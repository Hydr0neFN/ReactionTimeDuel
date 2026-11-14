#include "screen_maint.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "RtdNetConfig.h"
#include "RtdService.h"
#include "diag.h"
#include "net.h"
#include "ota.h"
#include "screen_settings.h"
#include "theme.h"
#include "ui_app.h"

static const uint32_t kRefreshMs = 500;    /* visible page refresh period */
static const uint32_t kStaleMs = 3000;     /* no ServicePacket for this long: "No data from the host" */
static const uint32_t kHoldMs = 1500;      /* the debug switch ignores the report this long after a touch */
static const uint32_t kDbgRepeatMs = 150;  /* the debug Wi-Fi request has no acknowledge: send it 3 times */
static const int kDbgSends = 3;

enum Page { PG_UPDATE, PG_STICKS, PG_HOST, PG_DISPLAY, PG_COUNT };
static const char *kPageName[PG_COUNT] = { "UPDATE", "STICKS", "HOST", "DISPLAY" };

static lv_obj_t *s_scr;
static lv_obj_t *s_page[PG_COUNT];
static lv_obj_t *s_page_btn[PG_COUNT], *s_page_lbl[PG_COUNT];
static lv_obj_t *s_btn_close;
static int s_cur_page;
static bool s_force;
static uint32_t s_last_ms;

/* UPDATE page */
static lv_obj_t *s_own_addr, *s_mode_line, *s_state, *s_bar;
static lv_obj_t *s_btn_mode, *s_lbl_mode, *s_btn_sticks;

/* STICKS page: columns 1..12 of rows 0..3 (column 0 is the colour dot) */
struct Col {
    const char *title;
    lv_coord_t w;
};
static const Col kCols[] = {
    { "", 28 },     { "LINK", 60 }, { "AGE", 66 },   { "FW", 66 },   { "FRMS", 66 },
    { "TXF", 50 },  { "DROP", 72 }, { "FAIL", 64 },  { "BAD", 60 },  { "LOOP", 66 },
    { "GAP", 60 },  { "STATE", 74 }, { "MPU", 56 },
};
static const int kNCols = (int)(sizeof(kCols) / sizeof(kCols[0]));
static const lv_coord_t kTableX = 4;
static lv_obj_t *s_cell[RTD_MAX_PLAYERS][13];
static lv_obj_t *s_vals_sticks, *s_nodata_sticks;

/* HOST page */
enum { H_UPTIME, H_FW, H_RESET, H_HEAP, H_LOOP, H_GAMES, H_RX, H_DROP, H_BAD, H_BCAST, H_AP, H_CLIENTS, H_TRACE, H_N };
static lv_obj_t *s_host_val[H_N];
static lv_obj_t *s_vals_host, *s_nodata_host, *s_dbg_sw;
static bool s_dbg_touched;
static uint32_t s_dbg_ms;
static uint8_t s_dbg_val;
static int s_dbg_left;
static lv_timer_t *s_dbg_timer;

/* DISPLAY page */
enum { D_FW, D_LINK, D_SNAP, D_RXDROP, D_TXFAIL, D_BAD, D_UI, D_HEAP, D_RESET, D_TOUCH, D_STA, D_OTA, D_AGE, D_FRAMES, D_FAILS, D_N };
static lv_obj_t *s_disp_val[D_N];

static uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- small helpers ---- */

/* Label text and colour are only written when they change (no redraw without a reason). */
static void put(lv_obj_t *l, const char *txt, uint32_t color = TH_TEXT)
{
    if (strcmp(lv_label_get_text(l), txt) != 0) lv_label_set_text(l, txt);
    lv_color_t c = th_color(color);
    if (lv_obj_get_style_text_color(l, LV_PART_MAIN).full != c.full) lv_obj_set_style_text_color(l, c, 0);
}

static void enable(lv_obj_t *b, bool on)
{
    if (on) {
        lv_obj_clear_state(b, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(b, LV_STATE_DISABLED);
    }
}

static void dim(lv_obj_t *o, bool on)
{
    lv_opa_t want = on ? LV_OPA_40 : LV_OPA_COVER;
    if (lv_obj_get_style_opa(o, LV_PART_MAIN) != want) lv_obj_set_style_opa(o, want, 0);
}

static void show_flag(lv_obj_t *o, bool on)
{
    if (on == !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    if (on) {
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Fixed-width, left-aligned, clipped label at (x, y). */
static lv_obj_t *cell(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, uint32_t color, const char *txt,
                      const lv_font_t *font = TH_FONT20)
{
    lv_obj_t *l = th_label(parent, font, color, txt);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    return l;
}

/* Transparent full-page layer for the value labels of a page (dimmed as one when the host is silent). */
static lv_obj_t *layer(lv_obj_t *page)
{
    lv_obj_t *o = th_box(page, TH_SCREEN_W, TH_SCREEN_H - 64);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* "name value" line: the name is static text on the page, the value goes into the layer. */
static lv_obj_t *kv(lv_obj_t *page, lv_obj_t *vals, lv_coord_t xn, lv_coord_t xv, lv_coord_t wv, lv_coord_t y,
                    const char *name)
{
    cell(page, xn, y, xv - xn - 8, TH_TEXT2, name);
    return cell(vals, xv, y, wv, TH_TEXT, "--");
}

static void ver_str(char *b, size_t n, uint8_t vb, uint8_t patch)
{
    if (vb) {
        snprintf(b, n, "V%u.%u.%u", (unsigned)(vb >> 4), (unsigned)(vb & 0xF), (unsigned)patch);
    } else {
        snprintf(b, n, "--");
    }
}

static void age_str(char *b, size_t n, uint8_t ds)
{
    if (ds == SVC_AGE_NEVER) {
        snprintf(b, n, "--");
    } else if (ds >= 254) {
        snprintf(b, n, ">25 s");
    } else {
        snprintf(b, n, "%u.%u s", (unsigned)(ds / 10), (unsigned)(ds % 10));
    }
}

/* ---- events ---- */

static void select_page(int p)
{
    s_cur_page = p;
    s_force = true;
    for (int i = 0; i < PG_COUNT; i++) {
        show_flag(s_page[i], i == p);
        if (i == p) {
            lv_obj_add_state(s_page_btn[i], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_page_btn[i], LV_STATE_CHECKED);
        }
        lv_obj_set_style_text_color(s_page_lbl[i], th_color(i == p ? TH_BG : TH_TEXT), 0);
    }
}

static void page_cb(lv_event_t *e) { select_page((int)(uintptr_t)lv_event_get_user_data(e)); }

static void close_cb(lv_event_t *) { ui_close_service(); }

static void mode_cb(lv_event_t *)
{
    bool in_maint = ui_link().state().phase == PH_MAINT;
    ui_confirm(ACT_MAINTENANCE, in_maint ? 0 : 1, in_maint ? "Leave update mode?" : "Enter update mode?");
}

static void sticks_cb(lv_event_t *) { ui_confirm(ACT_OTA_STICKS, 0, "Update the joysticks now?"); }

/* The radio has no acknowledge for the debug request: first send now, two repeats from the timer. */
static void dbg_timer_cb(lv_timer_t *t)
{
    ui_send_svc(SVC_OP_DEBUG_WIFI, s_dbg_val);
    if (--s_dbg_left <= 0) lv_timer_pause(t);
}

static void dbg_cb(lv_event_t *e)
{
    s_dbg_val = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0;
    s_dbg_touched = true;
    s_dbg_ms = now_ms();
    ui_send_svc(SVC_OP_DEBUG_WIFI, s_dbg_val);
    s_dbg_left = kDbgSends - 1;
    lv_timer_reset(s_dbg_timer);
    lv_timer_resume(s_dbg_timer);
}

/* ---- build ---- */

static lv_obj_t *header_btn(lv_obj_t *head, lv_coord_t x, lv_coord_t w, const char *text, lv_event_cb_t cb,
                            void *ud, lv_obj_t **lbl)
{
    lv_obj_t *b = lv_btn_create(head);
    lv_obj_set_size(b, w, 56);
    lv_obj_set_pos(b, x, 4);
    lv_obj_set_style_bg_color(b, th_color(TH_ORANGE), LV_STATE_CHECKED);
    lv_obj_t *l = th_label(b, TH_FONT20, TH_TEXT, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (lbl) *lbl = l;
    return b;
}

static void build_update(lv_obj_t *p)
{
    lv_obj_t *panel = th_panel(p, 760, 168);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel, 6, 0);

    char buf[96];
    snprintf(buf, sizeof(buf), "Wi-Fi  %s", RTD_AP_SSID);
    th_label(panel, TH_FONT28, TH_TEXT, buf);
    snprintf(buf, sizeof(buf), "Password  %s", RTD_AP_PASS);
    th_label(panel, TH_FONT28, TH_TEXT, buf);
    snprintf(buf, sizeof(buf), "http://%s", RTD_HOST_IP);
    th_label(panel, TH_FONT28, TH_ORANGE, buf);
    s_own_addr = th_label(panel, TH_FONT20, TH_TEXT2, "Display address: not connected");

    s_mode_line = th_label(p, TH_FONT28, TH_TEXT, "Update mode: OFF");
    lv_obj_align(s_mode_line, LV_ALIGN_TOP_MID, 0, 182);
    s_state = th_label(p, TH_FONT28, TH_TEXT, "");
    lv_obj_align(s_state, LV_ALIGN_TOP_MID, 0, 222);

    s_bar = lv_bar_create(p);
    lv_obj_set_size(s_bar, 560, 22);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 270);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_bg_color(s_bar, th_color(TH_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, th_color(TH_ORANGE), LV_PART_INDICATOR);

    s_btn_mode = lv_btn_create(p);
    lv_obj_set_size(s_btn_mode, 360, 64);
    lv_obj_set_pos(s_btn_mode, 30, 312);
    s_lbl_mode = th_label(s_btn_mode, TH_FONT20, TH_TEXT, "START UPDATE MODE");
    lv_obj_center(s_lbl_mode);
    lv_obj_add_event_cb(s_btn_mode, mode_cb, LV_EVENT_CLICKED, NULL);

    s_btn_sticks = lv_btn_create(p);
    lv_obj_set_size(s_btn_sticks, 360, 64);
    lv_obj_set_pos(s_btn_sticks, 410, 312);
    lv_obj_center(th_label(s_btn_sticks, TH_FONT20, TH_TEXT, "UPDATE JOYSTICKS"));
    lv_obj_add_event_cb(s_btn_sticks, sticks_cb, LV_EVENT_CLICKED, NULL);
}

static void build_sticks(lv_obj_t *p)
{
    lv_coord_t x = kTableX;
    for (int c = 0; c < kNCols; c++) {
        if (kCols[c].title[0]) cell(p, x, 8, kCols[c].w, TH_TEXT2, kCols[c].title);
        x = (lv_coord_t)(x + kCols[c].w);
    }
    s_vals_sticks = layer(p);
    for (int r = 0; r < RTD_MAX_PLAYERS; r++) {
        lv_coord_t y = (lv_coord_t)(52 + r * 62);
        lv_obj_t *dot = th_box(p, 24, 24);
        lv_obj_set_pos(dot, kTableX + 2, y);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, th_stick((uint8_t)(r + 1)), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        x = (lv_coord_t)(kTableX + kCols[0].w);
        for (int c = 1; c < kNCols; c++) {
            s_cell[r][c - 1] = cell(s_vals_sticks, x, y, kCols[c].w, TH_TEXT, "--");
            x = (lv_coord_t)(x + kCols[c].w);
        }
    }
    lv_obj_t *note = cell(p, 8, 304, 784, TH_TEXT2,
                          "TXF = host sends that failed. DROP, FAIL, BAD, LOOP ms, GAP s = reported by the joystick "
                          "while it is idle.");
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    s_nodata_sticks = cell(p, 8, 368, 784, TH_TEXT2, "No data from the host", TH_FONT28);
    lv_obj_add_flag(s_nodata_sticks, LV_OBJ_FLAG_HIDDEN);
}

static void build_host(lv_obj_t *p)
{
    s_vals_host = layer(p);
    static const char *kNames[H_N] = {
        "Uptime h:mm", "Firmware",       "Reset reason",    "Heap KB free / low", "Longest loop",
        "Games total", "Frames received", "Ring drops",      "Bad frames",         "Failed broadcasts",
        "Access point", "AP clients",     "Trace",
    };
    for (int i = 0; i < H_N; i++) {
        bool left = i < H_RX;
        int row = left ? i : i - H_RX;
        lv_coord_t y = (lv_coord_t)(12 + row * 36);
        s_host_val[i] = left ? kv(p, s_vals_host, 16, 250, 150, y, kNames[i])
                             : kv(p, s_vals_host, 420, 640, 150, y, kNames[i]);
    }

    cell(p, 16, 292, 220, TH_TEXT, "DEBUG WI-FI");
    s_dbg_sw = lv_switch_create(p);
    lv_obj_set_size(s_dbg_sw, 100, 56);
    lv_obj_set_pos(s_dbg_sw, 250, 280);
    lv_obj_set_style_bg_color(s_dbg_sw, th_color(TH_ORANGE), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_dbg_sw, dbg_cb, LV_EVENT_VALUE_CHANGED, NULL);
    cell(p, 380, 292, 410, TH_TEXT2, "Takes effect at the next host start");

    s_nodata_host = cell(p, 16, 352, 770, TH_TEXT2, "No data from the host", TH_FONT28);
    lv_obj_add_flag(s_nodata_host, LV_OBJ_FLAG_HIDDEN);

    s_dbg_timer = lv_timer_create(dbg_timer_cb, kDbgRepeatMs, NULL);
    lv_timer_pause(s_dbg_timer);
}

static void build_display(lv_obj_t *p)
{
    lv_obj_t *vals = layer(p);
    static const char *kLeft[] = { "Firmware", "Link", "Snapshots / 10 s", "RX drops", "TX fails", "Bad frames",
                                   "UI max pass ms" };
    static const char *kRight[] = { "Free heap KB", "Reset reason", "Touch", "Station", "Display OTA" };
    static const char *kView[] = { "Age", "Frames", "Failed sends" };
    for (int i = 0; i < 7; i++) {
        s_disp_val[D_FW + i] = kv(p, vals, 16, 250, 150, (lv_coord_t)(12 + i * 34), kLeft[i]);
    }
    for (int i = 0; i < 5; i++) {
        s_disp_val[D_HEAP + i] = kv(p, vals, 420, 620, 170, (lv_coord_t)(12 + i * 34), kRight[i]);
    }
    cell(p, 420, (lv_coord_t)(12 + 5 * 34 + 6), 370, TH_ORANGE, "Host's view of the display");
    for (int i = 0; i < 3; i++) {
        s_disp_val[D_AGE + i] = kv(p, vals, 420, 620, 170, (lv_coord_t)(12 + (6 + i) * 34 + 6), kView[i]);
    }
}

lv_obj_t *screen_maint_init()
{
    s_scr = th_screen();

    lv_obj_t *head = th_box(s_scr, TH_SCREEN_W, 64);
    lv_obj_t *t = th_label(head, TH_FONT28, TH_ORANGE, "SERVICE");
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 12, 0);

    static const lv_coord_t kX[PG_COUNT] = { 160, 278, 396, 494 };
    static const lv_coord_t kW[PG_COUNT] = { 112, 112, 92, 124 };
    for (int i = 0; i < PG_COUNT; i++) {
        s_page_btn[i] = header_btn(head, kX[i], kW[i], kPageName[i], page_cb, (void *)(uintptr_t)i, &s_page_lbl[i]);
    }
    s_btn_close = header_btn(head, 668, 120, "CLOSE", close_cb, NULL, NULL);

    for (int i = 0; i < PG_COUNT; i++) {
        s_page[i] = th_box(s_scr, TH_SCREEN_W, TH_SCREEN_H - 64);
        lv_obj_set_pos(s_page[i], 0, 64);
        lv_obj_clear_flag(s_page[i], LV_OBJ_FLAG_CLICKABLE);
    }
    build_update(s_page[PG_UPDATE]);
    build_sticks(s_page[PG_STICKS]);
    build_host(s_page[PG_HOST]);
    build_display(s_page[PG_DISPLAY]);
    select_page(PG_UPDATE);
    return s_scr;
}

void screen_maint_open()
{
    select_page(PG_UPDATE);
    s_dbg_touched = false;
}

/* ---- refresh ---- */

static void refresh_update(const StatePacket &st)
{
    bool maint = st.phase == PH_MAINT;
    char buf[64];
    if (net_sta_connected()) {
        snprintf(buf, sizeof(buf), "Display address: 192.168.4.%u", (unsigned)net_ip_last_octet());
    } else {
        snprintf(buf, sizeof(buf), "Display address: not connected");
    }
    put(s_own_addr, buf, TH_TEXT2);
    put(s_mode_line, maint ? "Update mode: ON" : "Update mode: OFF", maint ? TH_ORANGE : TH_TEXT);

    const char *txt;
    uint8_t pct;
    if (st.ota_state == OTA_STICKS) {
        snprintf(buf, sizeof(buf), "Updating stick %u  %u %%", (unsigned)st.ota_target, (unsigned)st.ota_progress);
        txt = buf;
        pct = st.ota_progress;
    } else if (st.ota_state == OTA_DONE) {
        txt = "Joystick update done";
        pct = 100;
    } else if (st.ota_state == OTA_ERROR) {
        txt = "Joystick update failed";
        pct = st.ota_progress;
    } else if (!maint) {
        txt = "Update mode is off";
        pct = 0;
    } else if (!net_sta_connected()) {
        txt = "Waiting for the access point...";
        pct = 0;
    } else {
        switch (ota_state()) {
        case ODS_RECEIVING: txt = "Receiving display image..."; break;
        case ODS_DONE:      txt = "Image accepted, restarting"; break;
        case ODS_FAILED:    txt = "Image rejected"; break;
        default:            txt = "Display ready for update"; break;
        }
        pct = ota_progress();
    }
    put(s_state, txt);
    if (lv_bar_get_value(s_bar) != pct) lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);

    put(s_lbl_mode, maint ? "LEAVE UPDATE MODE" : "START UPDATE MODE");
    enable(s_btn_mode, maint || st.phase == PH_IDLE);
    enable(s_btn_sticks, (st.flags & STF_UPDATE_AVAILABLE) != 0);
}

static void refresh_sticks(const StatePacket &st, const ServicePacket *sp, bool have, bool fresh)
{
    dim(s_vals_sticks, !fresh);
    show_flag(s_nodata_sticks, !fresh);
    if (!have) return;
    char b[24];
    for (int r = 0; r < RTD_MAX_PLAYERS; r++) {
        const ServiceDevice &d = sp->dev[r];
        bool online = (st.sticks_online >> r) & 1;
        bool diag = (d.flags & SVD_DIAG) != 0;
        lv_obj_t **c = s_cell[r];
        put(c[0], online ? "ON" : "OFF", online ? TH_GREEN_WIN : TH_RED);
        age_str(b, sizeof(b), d.age_ds);
        put(c[1], b);
        if (st.stick_fw[r]) {   /* from the stick's hello: has the patch number */
            snprintf(b, sizeof(b), "%u.%u.%u", (unsigned)(st.stick_fw[r] >> 4), (unsigned)(st.stick_fw[r] & 0xF),
                     (unsigned)st.stick_fw_patch[r]);
        } else if (d.fw) {
            snprintf(b, sizeof(b), "%u.%u", (unsigned)(d.fw >> 4), (unsigned)(d.fw & 0xF));
        } else {
            snprintf(b, sizeof(b), "--");
        }
        put(c[2], b);
        snprintf(b, sizeof(b), "%u", (unsigned)d.frames);
        put(c[3], b);
        snprintf(b, sizeof(b), "%u", (unsigned)d.host_tx_fail);
        put(c[4], b);
        if (diag) {
            snprintf(b, sizeof(b), "%u", (unsigned)d.rx_drop);
            put(c[5], b);
            snprintf(b, sizeof(b), "%u", (unsigned)d.tx_fail);
            put(c[6], b);
            snprintf(b, sizeof(b), "%u", (unsigned)d.bad_frame);
            put(c[7], b);
            snprintf(b, sizeof(b), "%u", (unsigned)d.loop_max_ms);
            put(c[8], b);
            snprintf(b, sizeof(b), "%u.%u", (unsigned)(d.link_gap_ds / 10), (unsigned)(d.link_gap_ds % 10));
            put(c[9], b);
            snprintf(b, sizeof(b), "%u", (unsigned)d.extra);
            put(c[10], b);
            bool ok = (d.flags & SVD_SENSOR_OK) != 0;
            put(c[11], ok ? "OK" : "FAIL", ok ? TH_TEXT : TH_RED);
        } else {
            for (int k = 5; k <= 11; k++) put(c[k], "--");
        }
    }
}

static void refresh_host(const StatePacket &st, const ServicePacket *sp, bool have, bool fresh, uint32_t now)
{
    dim(s_vals_host, !fresh);
    show_flag(s_nodata_host, !fresh);
    if (!have) return;
    char b[40];
    unsigned up = sp->uptime_min;
    snprintf(b, sizeof(b), "%u:%02u", up / 60, up % 60);
    put(s_host_val[H_UPTIME], b);
    ver_str(b, sizeof(b), st.host_fw, st.host_fw_patch);
    put(s_host_val[H_FW], b);
    snprintf(b, sizeof(b), "%u %s", (unsigned)sp->reset_reason, diag_reset_name(sp->reset_reason));
    put(s_host_val[H_RESET], b);
    snprintf(b, sizeof(b), "%u / %u", (unsigned)sp->heap_kb, (unsigned)sp->heap_min_kb);
    put(s_host_val[H_HEAP], b);
    snprintf(b, sizeof(b), "%u ms", (unsigned)sp->loop_max_ms);
    put(s_host_val[H_LOOP], b);
    snprintf(b, sizeof(b), "%u", (unsigned)st.games_total);
    put(s_host_val[H_GAMES], b);
    snprintf(b, sizeof(b), "%u", (unsigned)sp->rx_frames);
    put(s_host_val[H_RX], b);
    snprintf(b, sizeof(b), "%u", (unsigned)sp->rx_drop);
    put(s_host_val[H_DROP], b);
    snprintf(b, sizeof(b), "%u", (unsigned)sp->bad_frame);
    put(s_host_val[H_BAD], b);
    snprintf(b, sizeof(b), "%u", (unsigned)sp->tx_fail_bcast);
    put(s_host_val[H_BCAST], b);
    put(s_host_val[H_AP], (sp->host_flags & SVH_AP_ON) ? "ON" : "OFF");
    snprintf(b, sizeof(b), "%u", (unsigned)sp->ap_clients);
    put(s_host_val[H_CLIENTS], b);
    put(s_host_val[H_TRACE], (sp->host_flags & SVH_TRACE) ? "ON" : "OFF");

    /* The switch follows the report unless it was touched a moment ago. */
    if (!s_dbg_touched || (uint32_t)(now - s_dbg_ms) >= kHoldMs) {
        s_dbg_touched = false;
        bool want = (sp->host_flags & SVH_DEBUG_WIFI) != 0;
        if (lv_obj_has_state(s_dbg_sw, LV_STATE_CHECKED) != want) {
            if (want) {
                lv_obj_add_state(s_dbg_sw, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(s_dbg_sw, LV_STATE_CHECKED);
            }
        }
    }
}

static void refresh_display(const ServicePacket *sp, bool fresh, bool link_up, uint32_t now)
{
    DiagStats d;
    diag_stats(d, now);
    char b[40];
    put(s_disp_val[D_FW], FW_VERSION_STRING);
    put(s_disp_val[D_LINK], link_up ? "UP" : "DOWN", link_up ? TH_GREEN_WIN : TH_RED);
    snprintf(b, sizeof(b), "%u", (unsigned)d.snap_10s);
    put(s_disp_val[D_SNAP], b);
    snprintf(b, sizeof(b), "%u", (unsigned)d.rx_drop);
    put(s_disp_val[D_RXDROP], b);
    snprintf(b, sizeof(b), "%u", (unsigned)d.tx_fail);
    put(s_disp_val[D_TXFAIL], b);
    snprintf(b, sizeof(b), "%u", (unsigned)d.bad_frame);
    put(s_disp_val[D_BAD], b);
    snprintf(b, sizeof(b), "%u", (unsigned)d.loop_peak_ms);
    put(s_disp_val[D_UI], b);
    snprintf(b, sizeof(b), "%u", (unsigned)d.heap_kb);
    put(s_disp_val[D_HEAP], b);
    snprintf(b, sizeof(b), "%d %s", d.reset_reason, diag_reset_name(d.reset_reason));
    put(s_disp_val[D_RESET], b);
    put(s_disp_val[D_TOUCH], d.touch_ok ? "OK" : "FAIL", d.touch_ok ? TH_TEXT : TH_RED);
    if (net_sta_connected()) {
        snprintf(b, sizeof(b), "192.168.4.%u", (unsigned)net_ip_last_octet());
    } else {
        snprintf(b, sizeof(b), "not connected");
    }
    put(s_disp_val[D_STA], b);
    const char *ota = "OFF";
    switch (ota_state()) {
    case ODS_READY:     ota = "READY"; break;
    case ODS_RECEIVING: ota = "RECEIVING"; break;
    case ODS_DONE:      ota = "DONE"; break;
    case ODS_FAILED:    ota = "FAILED"; break;
    default: break;
    }
    put(s_disp_val[D_OTA], ota);

    if (fresh) {
        const ServiceDevice &v = sp->dev[SVC_DEVICES - 1];
        age_str(b, sizeof(b), v.age_ds);
        put(s_disp_val[D_AGE], b);
        snprintf(b, sizeof(b), "%u", (unsigned)v.frames);
        put(s_disp_val[D_FRAMES], b);
        snprintf(b, sizeof(b), "%u", (unsigned)v.host_tx_fail);
        put(s_disp_val[D_FAILS], b);
    } else {
        for (int i = D_AGE; i <= D_FAILS; i++) put(s_disp_val[i], "--");
    }
}

void screen_maint_update(const StatePacket &st, bool link_up)
{
    uint32_t now = now_ms();
    if (!s_force && (uint32_t)(now - s_last_ms) < kRefreshMs) return;
    s_force = false;
    s_last_ms = now;

    enable(s_btn_close, st.phase != PH_MAINT); /* update mode is left with LEAVE UPDATE MODE */

    const ServicePacket *sp = NULL;
    uint32_t rx = 0;
    bool have = ui_service(&sp, &rx);
    bool fresh = have && (uint32_t)(now - rx) <= kStaleMs;

    switch (s_cur_page) {
    case PG_UPDATE:  refresh_update(st); break;
    case PG_STICKS:  refresh_sticks(st, sp, have, fresh); break;
    case PG_HOST:    refresh_host(st, sp, have, fresh, now); break;
    default:         refresh_display(sp, fresh, link_up, now); break;
    }
}
