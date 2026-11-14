#include "ui_app.h"

#include "diag.h"
#include "net.h"
#include "ota.h"
#include "screen_game.h"
#include "screen_idle.h"
#include "screen_maint.h"
#include "screen_settings.h"
#include "theme.h"

#include "RtdProtocol.h"
#include "RtdService.h"

#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

static const uint32_t kTimerMs = 20;
static const uint32_t kFadeMs = 300;
static const uint32_t kSettingsIdleMs = 30000;
static const uint32_t kInfoIntervalMs = 2000;
static const uint32_t kValidAfterMs = 10000;
/* The host sends no snapshots while it handles a large upload, so a gap shorter than this must not leave maintenance mode. */
static const uint32_t kMaintLostMs = 15000;
static const uint32_t kDiagHoldMs = 3000;    /* long press on the link icon that toggles the diagnostics panel */
static const uint32_t kDiagRefreshMs = 1000;

static DisplayLink *s_link;
static lv_obj_t *s_scr_idle, *s_scr_game, *s_scr_settings, *s_scr_maint;
static lv_obj_t *s_current;
static bool s_settings_open;
static bool s_service_open; /* the service screen was opened from the settings (outside PH_MAINT) */
static bool s_in_maint;
static bool s_valid_done;
static bool s_dirty = true;
static uint32_t s_start_ms, s_info_ms;
static uint32_t s_snap_ms; /* rx time of the last good snapshot */

static ServicePacket s_svc; /* last valid service report of the host */
static uint32_t s_svc_ms;
static bool s_svc_have;
static uint32_t s_svc_poll_ms;

static lv_obj_t *s_overlay;
static lv_obj_t *s_overlay_icon;
static bool s_overlay_on;

static lv_obj_t *s_diag_panel, *s_diag_label;
static bool s_diag_on;
static uint32_t s_diag_ms;

DisplayLink &ui_link() { return *s_link; }

static uint32_t now_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

bool ui_service(const ServicePacket **sp, uint32_t *rx_ms)
{
    if (sp) *sp = &s_svc;
    if (rx_ms) *rx_ms = s_svc_ms;
    return s_svc_have;
}

void ui_send_svc(uint8_t op, uint8_t arg)
{
    GamePacket pkt;
    buildPacket(&pkt, ID_HOST, ID_DISPLAY, CMD_SVC_REQ, (uint16_t)(((uint16_t)op << 8) | arg));
    net_ports().send((const uint8_t *)&pkt, PACKET_SIZE);
}

/* ---- link-lost overlay ---- */

static void diag_icon_attach(lv_obj_t *icon);

static const uint32_t kGearHintMs = 1500; /* time the "hold" hint stays before it fades */
static lv_obj_t *s_gear_hint;
static void gear_hint_opa(void *o, int32_t v);

static void overlay_create()
{
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, TH_SCREEN_W, TH_SCREEN_H);
    lv_obj_set_style_bg_color(s_overlay, th_color(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE); /* swallows touches while there is no host */
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    s_overlay_icon = lv_img_create(s_overlay);
    lv_img_set_src(s_overlay_icon, &ui_img_icon_link_off);
    lv_obj_align(s_overlay_icon, LV_ALIGN_CENTER, 0, -40);
    diag_icon_attach(s_overlay_icon); /* the chrome is covered while there is no host */

    lv_obj_t *l = th_label(s_overlay, TH_FONT28, TH_TEXT, "CONNECTING TO HOST...");
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 20);
}

static void overlay_icon_opa(void *o, int32_t v) { lv_obj_set_style_img_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

static void overlay_show(bool on)
{
    if (on == s_overlay_on) return;
    s_overlay_on = on;
    if (on) {
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        th_anim(s_overlay_icon, overlay_icon_opa, 255, 90, 800, true);
    } else {
        lv_anim_del(s_overlay_icon, overlay_icon_opa);
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- diagnostics panel (DESIGN.md 11.4): hidden, toggled by a 3 s press on a link icon ---- */

static void diag_panel_create()
{
    s_diag_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_diag_panel);
    lv_obj_set_size(s_diag_panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_diag_panel, th_color(TH_PANEL), 0);
    lv_obj_set_style_bg_opa(s_diag_panel, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_diag_panel, TH_PANEL_RADIUS, 0);
    lv_obj_set_style_pad_all(s_diag_panel, 10, 0);
    lv_obj_clear_flag(s_diag_panel, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_align(s_diag_panel, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_add_flag(s_diag_panel, LV_OBJ_FLAG_HIDDEN);

    s_diag_label = th_label(s_diag_panel, TH_FONT20, TH_TEXT, "");
    lv_obj_set_style_text_align(s_diag_label, LV_TEXT_ALIGN_LEFT, 0);
}

static void diag_panel_refresh(uint32_t now, bool up)
{
    s_diag_ms = now;
    DiagStats d;
    diag_stats(d, now);
    lv_label_set_text_fmt(s_diag_label,
                          "FW %s\n"
                          "Link %s  snap/10s %u\n"
                          "RX drop %u\n"
                          "TX fail %u\n"
                          "Bad frame %u\n"
                          "UI max %u ms\n"
                          "Heap %u KB\n"
                          "Reset %d %s",
                          FW_VERSION_STRING, up ? "UP" : "DOWN", (unsigned)d.snap_10s, (unsigned)d.rx_drop,
                          (unsigned)d.tx_fail, (unsigned)d.bad_frame, (unsigned)d.loop_peak_ms,
                          (unsigned)d.heap_kb, d.reset_reason, diag_reset_name(d.reset_reason));
}

static void diag_panel_toggle()
{
    s_diag_on = !s_diag_on;
    if (s_diag_on) {
        diag_panel_refresh(now_ms(), s_link->linkUp());
        lv_obj_clear_flag(s_diag_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_diag_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

/* LONG_PRESSED fires after 1 s (touch driver); LONG_PRESSED_REPEAT keeps firing while the finger stays, so the
 * 3 s are counted from PRESSED. One toggle per press. */
static void diag_icon_event(lv_event_t *e)
{
    static uint32_t press_ms;
    static bool fired;
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        press_ms = now_ms();
        fired = false;
        break;
    case LV_EVENT_LONG_PRESSED_REPEAT:
        if (!fired && (uint32_t)(now_ms() - press_ms) >= kDiagHoldMs) {
            fired = true;
            diag_panel_toggle();
        }
        break;
    default:
        /* LV_EVENT_ALL also delivers PRESSING and the draw events during the press: ignore them.
         * The next PRESSED starts a new count. */
        break;
    }
}

static void diag_icon_attach(lv_obj_t *icon)
{
    lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(icon, 8); /* the icon is 40 px; stays clear of the gear's own click area */
    lv_obj_add_event_cb(icon, diag_icon_event, LV_EVENT_ALL, NULL);
}

/* ---- chrome icons ---- */

static void gear_long_press(lv_event_t *)
{
    if (s_gear_hint) {
        lv_anim_del(s_gear_hint, gear_hint_opa);
        lv_obj_add_flag(s_gear_hint, LV_OBJ_FLAG_HIDDEN);
    }
    ui_open_settings();
}

/* A tap does not open the settings (visitors). It shows a hint, so that the operator sees that the
 * touch works and what to do. */
static void gear_hint_opa(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }

static void gear_hint_ready(lv_anim_t *a) { lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN); }

static void gear_hint_create()
{
    s_gear_hint = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_gear_hint);
    lv_obj_set_size(s_gear_hint, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_bg_color(s_gear_hint, th_color(TH_PANEL), 0);
    lv_obj_set_style_bg_opa(s_gear_hint, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_gear_hint, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_gear_hint, 16, 0);
    lv_obj_clear_flag(s_gear_hint, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_align(s_gear_hint, LV_ALIGN_BOTTOM_RIGHT, -16, -68); /* above the gear */
    lv_obj_t *t = th_label(s_gear_hint, TH_FONT20, TH_TEXT, "HOLD 1 S FOR SETTINGS");
    lv_obj_center(t);
    lv_obj_add_flag(s_gear_hint, LV_OBJ_FLAG_HIDDEN);
}

static void gear_short_click(lv_event_t *)
{
    if (!s_gear_hint) return;
    lv_anim_del(s_gear_hint, gear_hint_opa);
    lv_obj_set_style_opa(s_gear_hint, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_gear_hint, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_gear_hint);
    lv_anim_set_exec_cb(&a, gear_hint_opa);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_delay(&a, kGearHintMs);
    lv_anim_set_time(&a, kFadeMs);
    lv_anim_set_ready_cb(&a, gear_hint_ready);
    lv_anim_start(&a);
}

void ui_chrome_create(lv_obj_t *screen, Chrome &c, lv_opa_t opa)
{
    c.gear = lv_img_create(screen);
    lv_img_set_src(c.gear, &ui_img_icon_gear);
    lv_obj_align(c.gear, LV_ALIGN_BOTTOM_RIGHT, -16, -14);
    lv_obj_set_style_img_opa(c.gear, opa, 0);
    lv_obj_add_flag(c.gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(c.gear, 24);
    lv_obj_add_event_cb(c.gear, gear_long_press, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(c.gear, gear_short_click, LV_EVENT_SHORT_CLICKED, NULL);
    /* Orange while the finger is on it: shows that the press is seen. */
    lv_obj_set_style_img_recolor(c.gear, th_color(TH_ORANGE), LV_STATE_PRESSED);
    lv_obj_set_style_img_recolor_opa(c.gear, LV_OPA_COVER, LV_STATE_PRESSED);

    c.link_icon = lv_img_create(screen);
    lv_img_set_src(c.link_icon, &ui_img_icon_link_on);
    lv_obj_align(c.link_icon, LV_ALIGN_BOTTOM_RIGHT, -16 - 44 - 14, -16);
    lv_obj_set_style_img_opa(c.link_icon, opa, 0);
    diag_icon_attach(c.link_icon);

    c.badge = lv_obj_create(screen);
    lv_obj_remove_style_all(c.badge);
    lv_obj_set_size(c.badge, LV_SIZE_CONTENT, 32);
    lv_obj_set_style_bg_color(c.badge, th_color(TH_ORANGE), 0);
    lv_obj_set_style_bg_opa(c.badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c.badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(c.badge, 14, 0);
    lv_obj_align(c.badge, LV_ALIGN_BOTTOM_RIGHT, -16 - 44 - 14 - 40 - 14, -16);
    lv_obj_t *t = th_label(c.badge, TH_FONT20, TH_BG, "UPDATE AVAILABLE");
    lv_obj_center(t);
    lv_obj_add_flag(c.badge, LV_OBJ_FLAG_HIDDEN);
}

void ui_chrome_update(Chrome &c, bool link_up, bool update_available)
{
    lv_img_set_src(c.link_icon, link_up ? &ui_img_icon_link_on : &ui_img_icon_link_off);
    if (update_available) {
        lv_obj_clear_flag(c.badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(c.badge, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- screen switching ---- */

static void show(lv_obj_t *scr)
{
    if (scr == s_current) return;
    s_current = scr;
    s_dirty = true;
    if (scr == s_scr_maint) {
        screen_maint_open(); /* UPDATE page first */
        s_svc_poll_ms = now_ms() - SVC_POLL_MS; /* first request at once */
    }
    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_ON, kFadeMs, 0, false);
}

void ui_open_settings()
{
    if (!s_link->hasState() || s_in_maint) return;
    s_settings_open = true;
    lv_disp_trig_activity(NULL); /* restart the 30 s idle timer */
    screen_settings_open();
    show(s_scr_settings);
}

void ui_close_settings()
{
    s_settings_open = false;
    s_dirty = true;
}

void ui_open_service()
{
    if (!s_link->hasState() || s_link->state().phase != PH_IDLE) return;
    s_settings_open = false;
    s_service_open = true;
}

void ui_close_service()
{
    s_service_open = false;
    s_dirty = true;
}

static lv_obj_t *screen_for_phase(uint8_t phase)
{
    if (phase == PH_MAINT) return s_scr_maint;
    if (phase == PH_IDLE) return s_scr_idle;
    return s_scr_game;
}

/*
 * Maintenance mode follows the host (DESIGN.md 9.1, 9.5). Entering is immediate. Leaving needs
 * ota_server_stop() (httpd_stop blocks until the handler thread ends) and esp_wifi_disconnect(), and the LVGL
 * task holds the LVGL mutex here, so leaving is refused while an upload runs and retried on the next tick.
 */
static void maint_enter()
{
    if (s_in_maint) return;
    s_in_maint = true;
    net_sta_enable(true);
    ota_server_start();
}

static void maint_leave_if_wanted(bool wanted)
{
    if (!s_in_maint || !wanted || ota_busy()) return;
    if (!ota_server_stop()) return; /* a handler started in between: try again later */
    net_sta_enable(false);
    s_in_maint = false;
}

/* ---- 20 ms timer: queue -> DisplayLink -> render ---- */

static void drain_queue()
{
    NetFrame f;
    while (net_pop(f)) {
        net_learn_host(f);
        if (f.len == SVC_SIZE) {
            /* Service report: never goes to DisplayLink and is no bad snapshot. */
            if (validateService(f.data, f.len)) {
                memcpy(&s_svc, f.data, SVC_SIZE);
                s_svc_ms = f.rx_ms;
                s_svc_have = true;
            }
            continue;
        }
        if (f.len != STATE_SIZE) continue;
        if (s_link->onPacket(f.data, f.len, f.rx_ms)) {
            s_dirty = true;
            s_snap_ms = f.rx_ms;
            diag_snapshot(f.rx_ms);
        } else {
            diag_bad_frame();
        }
    }
}

static void render(const StatePacket &st, bool up)
{
    if (s_current == s_scr_idle) {
        screen_idle_update(st, up);
    } else if (s_current == s_scr_game) {
        screen_game_update(st, up);
    } else if (s_current == s_scr_settings) {
        screen_settings_update(st, up);
    }
}

static void timer_cb(lv_timer_t *)
{
    uint32_t t0 = now_ms();
    drain_queue();
    s_link->tick();
    net_tick();

    uint32_t now = now_ms();
    bool up = s_link->linkUp();
    const StatePacket &st = s_link->state();

    static bool last_up = false;
    if (up != last_up) {
        last_up = up;
        s_dirty = true;
    }
    overlay_show(!up);

    if (up && st.phase == PH_MAINT) {
        maint_enter();
    } else {
        /* Leave on a snapshot that is not PH_MAINT, or after kMaintLostMs without any snapshot. A short gap
         * (up == false for 2 s) keeps the mode and the station. */
        maint_leave_if_wanted(st.phase != PH_MAINT || (uint32_t)(now - s_snap_ms) > kMaintLostMs);
    }
    if (s_in_maint) {
        s_settings_open = false;
        if ((uint32_t)(now - s_info_ms) >= kInfoIntervalMs) {
            s_info_ms = now;
            uint8_t ods = net_sta_connected() ? (uint8_t)ota_state() : (uint8_t)ODS_OFF;
            s_link->sendInfo(net_ip_last_octet(), ods);
        }
    }

    if (s_settings_open && lv_disp_get_inactive_time(NULL) > kSettingsIdleMs) {
        s_settings_open = false;
    }
    /* A game phase closes the service screen; PH_MAINT always shows it. */
    if (st.phase != PH_IDLE && st.phase != PH_MAINT) s_service_open = false;
    lv_obj_t *target;
    if (st.phase == PH_MAINT || s_service_open) {
        target = s_scr_maint;
    } else {
        target = s_settings_open ? s_scr_settings : screen_for_phase(st.phase);
    }
    show(target);

    if (s_current == s_scr_maint) {
        /* The host reports only while the last request is younger than SVC_HOLD_MS. */
        if (up && !ota_busy() && (uint32_t)(now - s_svc_poll_ms) >= SVC_POLL_MS) {
            s_svc_poll_ms = now;
            ui_send_svc(SVC_OP_POLL, 0);
        }
        screen_maint_update(st, up);
    } else if (s_dirty) {
        render(st, up);
    }
    s_dirty = false;

    if (!s_valid_done && (uint32_t)(now - s_start_ms) >= kValidAfterMs) {
        s_valid_done = true;
        ota_mark_valid();
    }

    diag_tick(now, st.phase);
    if (s_diag_on && (uint32_t)(now - s_diag_ms) >= kDiagRefreshMs) diag_panel_refresh(now, up);
    diag_loop_pass((uint32_t)(now_ms() - t0));
}

void ui_app_init(DisplayLink &link)
{
    s_link = &link;
    lv_theme_t *theme = lv_theme_default_init(lv_disp_get_default(), th_color(TH_ORANGE), th_color(TH_GREEN),
                                              true, TH_FONT20);
    lv_disp_set_theme(lv_disp_get_default(), theme);

    s_scr_idle = screen_idle_init();
    s_scr_game = screen_game_init();
    s_scr_settings = screen_settings_init();
    s_scr_maint = screen_maint_init();
    overlay_create();
    diag_panel_create(); /* after the link-lost overlay so that it stays visible above it */
    gear_hint_create();

    s_current = s_scr_idle;
    lv_scr_load(s_scr_idle);
    screen_idle_update(link.state(), false);
    overlay_show(true);

    s_start_ms = now_ms();
    lv_timer_create(timer_cb, kTimerMs, NULL);
}
