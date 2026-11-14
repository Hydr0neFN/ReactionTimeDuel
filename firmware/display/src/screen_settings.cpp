#include "screen_settings.h"

#include <stdio.h>
#include <string.h>

#include "theme.h"
#include "ui_app.h"

static const uint32_t kHoldMs = 1000;      /* a touched control ignores the snapshot for this long */
static const uint32_t kSendEveryMs = 150;  /* slider send rate while dragging */
static const lv_coord_t kRowH = 64;

static lv_obj_t *s_scr, *s_tabs;
static uint32_t s_touch_ms[CFG_SLOTS];
static uint32_t s_sent_ms[CFG_SLOTS];

struct Ctl {
    uint8_t key;
    lv_obj_t *obj;
    const uint8_t *vals; /* button groups: value of each button, 0xFF end */
    uint8_t kind;        /* 0 slider, 1 button group, 2 switch, 3 dropdown */
};
static Ctl s_ctl[12];
static int s_nctl;

/* status tab */
static lv_obj_t *s_st_state[RTD_MAX_PLAYERS], *s_st_buzz[RTD_MAX_PLAYERS];
static lv_obj_t *s_st_info, *s_btn_start, *s_btn_service;
static uint8_t s_pending_act, s_pending_arg;

static uint32_t now_ms() { return lv_tick_get(); }

static lv_obj_t *add_row(lv_obj_t *tab, const char *name)
{
    lv_obj_t *r = th_box(tab, 740, kRowH);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 16, 0);
    lv_obj_t *l = th_label(r, TH_FONT20, TH_TEXT, name);
    lv_obj_set_width(l, 220);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    return r;
}

static lv_obj_t *add_tab(const char *name)
{
    lv_obj_t *t = lv_tabview_add_tab(s_tabs, name);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(t, 6, 0);
    return t;
}

static void orange(lv_obj_t *o, lv_style_selector_t part)
{
    lv_obj_set_style_bg_color(o, th_color(TH_ORANGE), part);
}

/* ---- cfg controls ---- */

static void cfg_send(uint8_t key, uint8_t v)
{
    s_touch_ms[key] = s_sent_ms[key] = now_ms();
    ui_link().setCfg(key, v);
}

static void slider_cb(lv_event_t *e)
{
    uint8_t key = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    uint8_t v = (uint8_t)lv_slider_get_value(lv_event_get_target(e));
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        cfg_send(key, v);
        if (key == CFG_VOLUME) ui_link().sendAction(ACT_TEST_SOUND, 0);
    } else if (now_ms() - s_sent_ms[key] >= kSendEveryMs) {
        cfg_send(key, v);
    } else {
        s_touch_ms[key] = now_ms();
    }
}

static void add_slider(lv_obj_t *tab, const char *name, uint8_t key, int lo, int hi)
{
    lv_obj_t *r = add_row(tab, name);
    lv_obj_t *s = lv_slider_create(r);
    lv_obj_set_size(s, 420, 22);
    lv_obj_set_ext_click_area(s, 20);
    lv_slider_set_range(s, lo, hi);
    orange(s, LV_PART_INDICATOR);
    orange(s, LV_PART_KNOB);
    lv_obj_t *val = th_label(r, TH_FONT20, TH_TEXT2, "");
    (void)val;
    lv_obj_add_event_cb(s, slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)key);
    lv_obj_add_event_cb(s, slider_cb, LV_EVENT_RELEASED, (void *)(uintptr_t)key);
    s_ctl[s_nctl++] = { key, s, NULL, 0 };
}

static void group_cb(lv_event_t *e)
{
    Ctl *c = (Ctl *)lv_event_get_user_data(e);
    uint16_t id = lv_btnmatrix_get_selected_btn(c->obj);
    if (id != LV_BTNMATRIX_BTN_NONE) cfg_send(c->key, c->vals[id]);
}

static void add_group(lv_obj_t *tab, const char *name, uint8_t key, const char **map, const uint8_t *vals, int w)
{
    lv_obj_t *r = add_row(tab, name);
    lv_obj_t *b = lv_btnmatrix_create(r);
    lv_btnmatrix_set_map(b, map);
    lv_btnmatrix_set_btn_ctrl_all(b, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(b, true);
    lv_obj_set_size(b, w, 56);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    orange(b, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(b, th_color(TH_BG), LV_PART_ITEMS | LV_STATE_CHECKED);
    s_ctl[s_nctl] = { key, b, vals, 1 };
    lv_obj_add_event_cb(b, group_cb, LV_EVENT_VALUE_CHANGED, &s_ctl[s_nctl]);
    s_nctl++;
}

static void switch_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    cfg_send((uint8_t)(uintptr_t)lv_event_get_user_data(e), lv_obj_has_state(s, LV_STATE_CHECKED) ? 1 : 0);
}

static void dropdown_cb(lv_event_t *e)
{
    cfg_send((uint8_t)(uintptr_t)lv_event_get_user_data(e), (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e)));
}

/* ---- actions and the confirmation box ---- */

static void confirm_cb(lv_event_t *e)
{
    lv_obj_t *box = lv_event_get_current_target(e);
    if (lv_msgbox_get_active_btn(box) == 0) ui_link().sendAction(s_pending_act, s_pending_arg);
    lv_msgbox_close(box);
}

void ui_confirm(uint8_t act, uint8_t arg, const char *text)
{
    static const char *btns[] = { "YES", "NO", "" };
    s_pending_act = act;
    s_pending_arg = arg;
    lv_obj_t *m = lv_msgbox_create(NULL, "CONFIRM", text, btns, false);
    lv_obj_set_size(m, 460, 220);
    lv_obj_center(m);
    lv_obj_add_event_cb(m, confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void act_cb(lv_event_t *e)
{
    uintptr_t u = (uintptr_t)lv_event_get_user_data(e);
    uint8_t act = (uint8_t)(u >> 8), arg = (uint8_t)(u & 0xFF);
    switch (act) {
    case ACT_ABORT_GAME:    ui_confirm(act, arg, "Abort the running game?"); break;
    case ACT_FACTORY_RESET: ui_confirm(act, arg, "Restore all settings to default?"); break;
    default: ui_link().sendAction(act, arg); break;
    }
}

static lv_obj_t *add_btn(lv_obj_t *parent, const char *text, uint8_t act, uint8_t arg, int w)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, 56);
    lv_obj_t *l = th_label(b, TH_FONT20, TH_TEXT, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, act_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)(((uintptr_t)act << 8) | arg));
    return b;
}

static void back_cb(lv_event_t *) { ui_close_settings(); }

static void service_cb(lv_event_t *) { ui_open_service(); }

static void enable(lv_obj_t *b, bool on)
{
    if (on) {
        lv_obj_clear_state(b, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(b, LV_STATE_DISABLED);
    }
}

/* ---- screen ---- */

lv_obj_t *screen_settings_init()
{
    s_scr = th_screen();

    lv_obj_t *head = th_box(s_scr, TH_SCREEN_W, 64);
    lv_obj_t *back = lv_btn_create(head);
    lv_obj_set_size(back, 120, 56);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t *bl = th_label(back, TH_FONT_SYMBOL, TH_TEXT, LV_SYMBOL_LEFT);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *title = th_label(head, TH_FONT28, TH_TEXT, "SETTINGS");
    lv_obj_center(title);

    s_tabs = lv_tabview_create(s_scr, LV_DIR_TOP, 56);
    lv_obj_set_size(s_tabs, TH_SCREEN_W, TH_SCREEN_H - 64);
    lv_obj_set_pos(s_tabs, 0, 64);
    lv_obj_set_style_bg_color(s_tabs, th_color(TH_BG), 0);
    lv_obj_t *tb = lv_tabview_get_tab_btns(s_tabs);
    lv_obj_set_style_text_font(tb, TH_FONT20, 0);
    lv_obj_set_style_text_color(tb, th_color(TH_ORANGE), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(tb, th_color(TH_ORANGE), LV_PART_ITEMS | LV_STATE_CHECKED);

    lv_obj_t *t1 = add_tab("SOUND & LIGHT");
    add_slider(t1, "Volume", CFG_VOLUME, 0, 10);
    add_slider(t1, "Ring brightness", CFG_RING_BRIGHT, 1, 10);
    add_slider(t1, "Strip brightness", CFG_STRIP_BRIGHT, 0, 10);
    lv_obj_t *r = add_row(t1, "Strip effect");
    lv_obj_t *dd = lv_dropdown_create(r);
    lv_dropdown_set_options(dd, "AUTO\nRAINBOW\nSPARKLE\nMETEOR\nCHASE\nBREATHING\nFIRE\nHANZE");
    lv_obj_set_size(dd, 300, 56);
    lv_obj_set_style_text_font(dd, TH_FONT_SYMBOL, LV_PART_INDICATOR);
    lv_obj_add_event_cb(dd, dropdown_cb, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)CFG_STRIP_MODE);
    s_ctl[s_nctl++] = { CFG_STRIP_MODE, dd, NULL, 3 };

    static const char *rounds_map[] = { "3", "5", "7", "10", "" };
    static const uint8_t rounds_v[] = { 3, 5, 7, 10, 0xFF };
    static const char *mode_map[] = { "MIXED", "REACTION", "SHAKE", "" };
    static const uint8_t mode_v[] = { 0, 1, 2, 0xFF };
    static const char *scoring_map[] = { "CLASSIC", "PLACES", "" };
    static const uint8_t scoring_v[] = { SCORING_CLASSIC, SCORING_PLACES, 0xFF };
    static const char *bonus_map[] = { "OFF", "LAST", "LAST 2", "" };
    static const uint8_t bonus_v[] = { BONUS_OFF, BONUS_LAST, BONUS_LAST_TWO, 0xFF };
    static const char *deuce_map[] = { "1", "2", "3", "" };
    static const uint8_t deuce_v[] = { 1, 2, 3, 0xFF };
    lv_obj_t *t2 = add_tab("GAME");
    lv_obj_set_scroll_dir(t2, LV_DIR_VER); /* 7 rows do not fit: the tab scrolls vertically, a sideways swipe still changes the tab */
    add_group(t2, "Rounds", CFG_ROUNDS, rounds_map, rounds_v, 360);
    add_group(t2, "Mode", CFG_GAME_MODE, mode_map, mode_v, 480);
    add_group(t2, "Scoring", CFG_SCORING, scoring_map, scoring_v, 360);
    add_group(t2, "Bonus", CFG_BONUS, bonus_map, bonus_v, 420);
    r = add_row(t2, "Voice guide");
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 100, 56);
    orange(sw, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)CFG_VOICE_GUIDE);
    s_ctl[s_nctl++] = { CFG_VOICE_GUIDE, sw, NULL, 2 };
    add_slider(t2, "Join time (s)", CFG_JOIN_TIME, 3, 10);
    add_group(t2, "Deuce lead", CFG_DEUCE_LEAD, deuce_map, deuce_v, 270);

    lv_obj_t *t3 = add_tab("STATUS");
    for (int i = 0; i < RTD_MAX_PLAYERS; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "Stick %d", i + 1);
        lv_obj_t *row = add_row(t3, nm);
        lv_obj_t *dot = th_box(row, 24, 24);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, th_stick((uint8_t)(i + 1)), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        s_st_state[i] = th_label(row, TH_FONT20, TH_TEXT, "");
        lv_obj_set_width(s_st_state[i], 300);
        lv_obj_set_style_text_align(s_st_state[i], LV_TEXT_ALIGN_LEFT, 0);
        s_st_buzz[i] = add_btn(row, "BUZZ", ACT_IDENTIFY, (uint8_t)(i + 1), 120);
    }
    s_st_info = th_label(t3, TH_FONT20, TH_TEXT2, "");
    lv_obj_set_style_text_align(s_st_info, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_t *b1 = th_box(t3, 740, 64);
    lv_obj_set_flex_flow(b1, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(b1, 12, 0);
    add_btn(b1, "RESET RECORD", ACT_RESET_BEST, 0, 220);
    s_btn_start = add_btn(b1, "START NOW", ACT_START_NOW, 0, 200);
    add_btn(b1, "ABORT GAME", ACT_ABORT_GAME, 0, 220);
    lv_obj_t *b2 = th_box(t3, 740, 64);
    lv_obj_set_flex_flow(b2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(b2, 12, 0);
    add_btn(b2, "RESTORE DEFAULTS", ACT_FACTORY_RESET, 0, 260);
    s_btn_service = lv_btn_create(b2);
    lv_obj_set_size(s_btn_service, 220, 56);
    lv_obj_center(th_label(s_btn_service, TH_FONT20, TH_TEXT, "SERVICE MENU"));
    lv_obj_add_event_cb(s_btn_service, service_cb, LV_EVENT_CLICKED, NULL);
    return s_scr;
}

void screen_settings_open()
{
    lv_tabview_set_act(s_tabs, 0, LV_ANIM_OFF);
    memset(s_touch_ms, 0, sizeof(s_touch_ms));
}

static void ver(char *b, size_t n, uint8_t vb, uint8_t patch)
{
    if (vb) {
        snprintf(b, n, "V%u.%u.%u", (unsigned)(vb >> 4), (unsigned)(vb & 0xF), (unsigned)patch);
    } else {
        snprintf(b, n, "--");
    }
}

void screen_settings_update(const StatePacket &st, bool)
{
    DisplayLink &lk = ui_link();
    uint32_t now = now_ms();
    for (int i = 0; i < s_nctl; i++) {
        Ctl &c = s_ctl[i];
        if (now - s_touch_ms[c.key] < kHoldMs && s_touch_ms[c.key]) continue;
        uint8_t v = lk.cfg(c.key);
        if (c.kind == 0 && lv_slider_get_value(c.obj) != v) {
            lv_slider_set_value(c.obj, v, LV_ANIM_OFF);
        } else if (c.kind == 2) {
            if (lv_obj_has_state(c.obj, LV_STATE_CHECKED) != (v != 0)) {
                if (v) lv_obj_add_state(c.obj, LV_STATE_CHECKED); else lv_obj_clear_state(c.obj, LV_STATE_CHECKED);
            }
        } else if (c.kind == 3 && lv_dropdown_get_selected(c.obj) != v) {
            lv_dropdown_set_selected(c.obj, v);
        } else if (c.kind == 1) {
            for (uint16_t k = 0; c.vals[k] != 0xFF; k++) {
                if (c.vals[k] == v && lv_btnmatrix_get_selected_btn(c.obj) != k) {
                    lv_btnmatrix_set_btn_ctrl(c.obj, k, LV_BTNMATRIX_CTRL_CHECKED);
                    lv_btnmatrix_set_selected_btn(c.obj, k);
                }
            }
        }
    }

    char vb[24];
    for (int i = 0; i < RTD_MAX_PLAYERS; i++) {
        bool on = (st.sticks_online >> i) & 1;
        ver(vb, sizeof(vb), st.stick_fw[i], st.stick_fw_patch[i]);
        char b[48];
        snprintf(b, sizeof(b), "%s   %s", on ? "ONLINE" : "OFFLINE", vb);
        if (strcmp(lv_label_get_text(s_st_state[i]), b) != 0) lv_label_set_text(s_st_state[i], b);
        enable(s_st_buzz[i], st.phase == PH_IDLE && on);
    }
    char h[24], info[160];
    ver(h, sizeof(h), st.host_fw, st.host_fw_patch);
    snprintf(vb, sizeof(vb), "V%u.%u.%u", FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH);
    snprintf(info, sizeof(info), "Host %s   Display %s   Best today %s   Record %s   Games %u", h, vb,
             st.best_session_ms == TIME_NONE ? "--" : "set", st.best_alltime_ms == TIME_NONE ? "--" : "set",
             (unsigned)st.games_total);
    if (st.best_session_ms != TIME_NONE || st.best_alltime_ms != TIME_NONE) {
        snprintf(info, sizeof(info), "Host %s   Display %s   Best today %u ms   Record %u ms   Games %u", h, vb,
                 (unsigned)(st.best_session_ms == TIME_NONE ? 0 : st.best_session_ms),
                 (unsigned)(st.best_alltime_ms == TIME_NONE ? 0 : st.best_alltime_ms), (unsigned)st.games_total);
    }
    if (strcmp(lv_label_get_text(s_st_info), info) != 0) lv_label_set_text(s_st_info, info);
    enable(s_btn_start, (st.flags & STF_CAN_START) != 0);
    enable(s_btn_service, st.phase == PH_IDLE); /* the service screen is only allowed while idle */
}
