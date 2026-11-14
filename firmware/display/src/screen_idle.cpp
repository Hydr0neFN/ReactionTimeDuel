#include "screen_idle.h"

#include <stdio.h>

#include "theme.h"
#include "ui_app.h"

static const int kCircles = 12;

static lv_obj_t *s_scr;
static lv_obj_t *s_logo, *s_pill;
static lv_obj_t *s_circle[kCircles];
static uint32_t s_circle_ms[kCircles];
static lv_obj_t *s_stat_today, *s_stat_record, *s_stat_games;
static lv_obj_t *s_dot[RTD_MAX_PLAYERS];
static Chrome s_chrome;

/* ---- drifting circles: one cheap object each, no shadow, opacity set once ---- */

static int circle_index(void *var)
{
    for (int i = 0; i < kCircles; i++) {
        if (s_circle[i] == var) return i;
    }
    return 0;
}

static void drift_loop_start(lv_anim_t *a)
{
    int i = circle_index(a->var);
    lv_anim_t n;
    lv_anim_init(&n);
    lv_anim_set_var(&n, s_circle[i]);
    lv_anim_set_exec_cb(&n, th_set_y);
    lv_anim_set_values(&n, TH_SCREEN_H + 20, -20);
    lv_anim_set_time(&n, s_circle_ms[i]);
    lv_anim_set_path_cb(&n, lv_anim_path_linear);
    lv_anim_set_repeat_count(&n, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&n);
}

static void drift_start(int i)
{
    lv_anim_del(s_circle[i], th_set_y);
    lv_coord_t y = lv_obj_get_y(s_circle[i]);
    uint32_t first = (uint32_t)((int64_t)s_circle_ms[i] * (y + 20) / (TH_SCREEN_H + 40));
    if (first < 200) first = 200;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_circle[i]);
    lv_anim_set_exec_cb(&a, th_set_y);
    lv_anim_set_values(&a, y, -20);
    lv_anim_set_time(&a, first);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_ready_cb(&a, drift_loop_start);
    lv_anim_start(&a);
}

static void anims_start(lv_event_t *)
{
    for (int i = 0; i < kCircles; i++) drift_start(i);
    th_anim(s_logo, th_set_y, 20, 26, 2000, true);
    th_anim(s_pill, th_set_opa, 255, 120, 600, true);
}

static void anims_stop(lv_event_t *)
{
    for (int i = 0; i < kCircles; i++) lv_anim_del(s_circle[i], th_set_y);
    lv_anim_del(s_logo, th_set_y);
    lv_anim_del(s_pill, th_set_opa);
}

/* ---- widgets ---- */

static lv_obj_t *stat_label(lv_obj_t *row)
{
    lv_obj_t *l = th_label(row, TH_FONT20, TH_TEXT, "");
    lv_label_set_recolor(l, true);
    return l;
}

static void set_stat(lv_obj_t *l, const char *caption, uint16_t value, const char *unit)
{
    char buf[64];
    if (value == TIME_NONE || value == 0) {
        snprintf(buf, sizeof(buf), "#8A9099 %s#  --", caption);
    } else {
        snprintf(buf, sizeof(buf), "#8A9099 %s#  %u%s", caption, (unsigned)value, unit);
    }
    lv_label_set_text(l, buf);
}

lv_obj_t *screen_idle_init()
{
    s_scr = th_screen();

    for (int i = 0; i < kCircles; i++) {
        lv_coord_t d = (lv_coord_t)lv_rand(6, 14);
        lv_obj_t *c = th_box(s_scr, d, d);
        lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(c, th_stick((uint8_t)(1 + i % 4)), 0);
        lv_obj_set_style_bg_opa(c, (lv_opa_t)lv_rand(77, 153), 0); /* 30..60 % */
        lv_obj_set_pos(c, (lv_coord_t)lv_rand(10, TH_SCREEN_W - 20), (lv_coord_t)lv_rand(0, TH_SCREEN_H));
        s_circle[i] = c;
        s_circle_ms[i] = lv_rand(6000, 12000);
    }

    s_logo = lv_img_create(s_scr);
    lv_img_set_src(s_logo, &ui_img_hanze_logo);
    lv_obj_align(s_logo, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *title = th_label(s_scr, TH_FONT40, TH_TEXT, "REACTION TIME DUEL");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 168);

    s_pill = th_box(s_scr, 580, 64);
    lv_obj_set_style_radius(s_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_pill, 3, 0);
    lv_obj_set_style_border_color(s_pill, th_color(TH_GREEN), 0);
    lv_obj_align(s_pill, LV_ALIGN_TOP_MID, 0, 238);
    lv_obj_t *pt = th_label(s_pill, TH_FONT28, TH_ORANGE, "PRESS YOUR BUTTON TO JOIN");
    lv_obj_center(pt);

    lv_obj_t *row = th_box(s_scr, 760, 30);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 44, 0);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 326);
    s_stat_today = stat_label(row);
    s_stat_record = stat_label(row);
    s_stat_games = stat_label(row);

    lv_obj_t *dots = th_box(s_scr, 200, 28);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dots, 20, 0);
    lv_obj_align(dots, LV_ALIGN_TOP_MID, 0, 384);
    for (int i = 0; i < RTD_MAX_PLAYERS; i++) {
        lv_obj_t *d = th_box(dots, 24, 24);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, th_stick((uint8_t)(i + 1)), 0);
        lv_obj_set_style_border_color(d, th_stick((uint8_t)(i + 1)), 0);
        lv_obj_set_style_border_width(d, 3, 0);
        s_dot[i] = d;
    }

    /* Link to the source code: 33 modules at 3 px each, with a white quiet zone. */
    static const char kRepoUrl[] = "https://github.com/Hydr0neFN/ReactionTimeDuel";
    lv_obj_t *qr = lv_qrcode_create(s_scr, 99, lv_color_black(), lv_color_white());
    lv_qrcode_update(qr, kRepoUrl, sizeof(kRepoUrl) - 1);
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 4, 0);
    lv_obj_align(qr, LV_ALIGN_BOTTOM_LEFT, 16, -14);
    lv_obj_t *qt = th_label(s_scr, TH_FONT20, TH_TEXT2, "Project Page");
    lv_obj_align_to(qt, qr, LV_ALIGN_OUT_RIGHT_BOTTOM, 12, 0);

    lv_obj_t *credit = th_label(s_scr, &lv_font_montserrat_16, TH_TEXT2,
                                "Made with love by Dogari A. and Juan Y.");
    lv_obj_align(credit, LV_ALIGN_BOTTOM_MID, 0, -16);

    ui_chrome_create(s_scr, s_chrome, LV_OPA_COVER);

    lv_obj_add_event_cb(s_scr, anims_start, LV_EVENT_SCREEN_LOAD_START, NULL);
    lv_obj_add_event_cb(s_scr, anims_stop, LV_EVENT_SCREEN_UNLOADED, NULL);
    return s_scr;
}

void screen_idle_update(const StatePacket &st, bool link_up)
{
    set_stat(s_stat_today, "BEST TODAY", st.best_session_ms, " ms");
    set_stat(s_stat_record, "RECORD", st.best_alltime_ms, " ms");
    set_stat(s_stat_games, "GAMES", st.games_total, "");

    for (int i = 0; i < RTD_MAX_PLAYERS; i++) {
        bool online = (st.sticks_online >> i) & 1;
        lv_obj_set_style_bg_opa(s_dot[i], online ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    ui_chrome_update(s_chrome, link_up, (st.flags & STF_UPDATE_AVAILABLE) != 0);
}
