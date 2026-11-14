#include "screen_game.h"

#include <stdio.h>
#include <string.h>

#include "theme.h"
#include "ui_app.h"

/* Ring centres of the SquareLine main screen, offsets from the screen centre. */
static const lv_coord_t kRingX[RTD_MAX_PLAYERS] = { -293, -92, 103, 295 };
static const lv_coord_t kRingY = -160;
static const lv_coord_t kCountdownY = 66;
static const int kConfetti = 40;

/* Score block of a player, inside its ring (offsets from the ring centre, which is at screen y 80, inner radius 71).
 * While the scores show, the V1 "PLAYER n" image is replaced by one line of text (Bold 20) at the top of the ring,
 * the score (Bold 64, white) sits in the middle and the leader gets an orange "1ST" tag below it. */
static const lv_coord_t kNameDy = -40;     /* "PLAYER n" text centre */
static const lv_coord_t kScoreDy = 8;      /* score digits centre */
static const lv_coord_t kRankDy = 50;      /* leader tag centre */
static const lv_coord_t kTimeDy = 6;       /* reaction time centre (replaces the score until the winner shows) */
static const lv_coord_t kUnitDy = 42;      /* "ms" below the time */
static const uint32_t kRingOff = 0x3A4048u; /* ring and dash of a player who did not join, while the scores show */
static const lv_coord_t kGainDx = 72;      /* "+N" centre: right of the ring, in the gap to the next ring */
static const int kGainRise = 30;
static const uint32_t kGainMs = 900;
static const uint32_t kPopMs = 200;
static const lv_coord_t kBonusX = 6;       /* bonus banner: left of the centre circle, below the time text of ring 1 */
static const lv_coord_t kBonusY = 208;
static const lv_coord_t kBonusPad = 10;

static lv_obj_t *s_panel[RTD_MAX_PLAYERS];
static lv_obj_t *s_pimg[RTD_MAX_PLAYERS];
static lv_obj_t *s_time[RTD_MAX_PLAYERS];
static lv_obj_t *s_unit[RTD_MAX_PLAYERS];
static lv_obj_t *s_arc[RTD_MAX_PLAYERS];
static lv_obj_t *s_pulse, *s_flash, *s_round, *s_newrec, *s_final;
static lv_obj_t *s_score[RTD_MAX_PLAYERS], *s_rank[RTD_MAX_PLAYERS], *s_gain[RTD_MAX_PLAYERS];
static lv_obj_t *s_name[RTD_MAX_PLAYERS], *s_dash[RTD_MAX_PLAYERS];
static lv_obj_t *s_bonus;
static lv_obj_t *s_winbox; /* frame of the WINNER box, in the colour of the winner's stick */
static bool s_bonus_on;
static lv_obj_t *s_confetti[kConfetti];
static bool s_confetti_on;
static Chrome s_chrome;

/* What was applied last: animations start on a change between two snapshots. */
static struct {
    bool valid;
    uint8_t phase, countdown, joined, winner, flags;
    uint8_t status[RTD_MAX_PLAYERS];
    bool pulsing[RTD_MAX_PLAYERS];
    bool prompted[RTD_MAX_PLAYERS];
    bool winpulse[RTD_MAX_PLAYERS];
    uint8_t score[RTD_MAX_PLAYERS]; /* last shown score of each player */
} s_prev;

static bool s_deuce_done;
static lv_timer_t *s_deuce_timer;
static StatePacket s_last;

/* ---- idempotent widget setters: touching a widget invalidates a full frame, so only on change ---- */

static void vis(lv_obj_t *o, bool on)
{
    if (!o || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) != on) return;
    if (on) {
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

static void txt(lv_obj_t *l, const char *s)
{
    if (strcmp(lv_label_get_text(l), s) != 0) lv_label_set_text(l, s);
}

static void tcol(lv_obj_t *l, uint32_t hex)
{
    lv_color_t c = th_color(hex);
    if (lv_obj_get_style_text_color(l, LV_PART_MAIN).full == c.full) return;
    lv_obj_set_style_text_color(l, c, 0);
}

static void tfont(lv_obj_t *l, const lv_font_t *f)
{
    if (lv_obj_get_style_text_font(l, LV_PART_MAIN) != f) lv_obj_set_style_text_font(l, f, 0);
}

static void set_x(void *o, int32_t v) { lv_obj_set_x((lv_obj_t *)o, (lv_coord_t)v); }

/* ---- animations ---- */

static void flash_ready(lv_anim_t *) { vis(s_flash, false); }

static void flash_go()
{
    lv_obj_move_foreground(s_flash);
    vis(s_flash, true);
    lv_anim_del(s_flash, th_set_bg_opa);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_flash);
    lv_anim_set_exec_cb(&a, th_set_bg_opa);
    lv_anim_set_values(&a, 180, 0);
    lv_anim_set_time(&a, 250);
    lv_anim_set_ready_cb(&a, flash_ready);
    lv_anim_start(&a);
}

static void ring_pulse(int i, bool on, bool &flag, int lo)
{
    if (on == flag) return;
    flag = on;
    lv_anim_del(s_panel[i], th_set_border_opa);
    lv_obj_set_style_border_opa(s_panel[i], LV_OPA_COVER, 0);
    if (on) th_anim(s_panel[i], th_set_border_opa, 255, lo, 600, true);
}

static void blink(lv_obj_t *o, bool on)
{
    if (on == !lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
    lv_anim_del(o, th_set_opa);
    lv_obj_set_style_opa(o, LV_OPA_COVER, 0);
    vis(o, on);
    if (on) th_anim(o, th_set_opa, 255, 0, 500, true, lv_anim_path_linear);
}

static void confetti(bool on)
{
    if (on == s_confetti_on) return;
    s_confetti_on = on;
    for (int i = 0; i < kConfetti; i++) {
        if (!on) {
            if (s_confetti[i]) lv_obj_del(s_confetti[i]);
            s_confetti[i] = NULL;
            continue;
        }
        static const uint32_t cols[] = { 0xFFFFFF, 0x4060FF, 0xFF0000, 0xFFFF00, TH_ORANGE, TH_GREEN_WIN };
        lv_obj_t *c = th_box(lv_scr_act() == NULL ? NULL : ui_MainScreen, 8, 12);
        lv_obj_set_style_radius(c, 2, 0);
        lv_obj_set_style_bg_color(c, th_color(cols[i % 6]), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_pos(c, (lv_coord_t)lv_rand(0, TH_SCREEN_W - 10), -20);
        s_confetti[i] = c;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, c);
        lv_anim_set_exec_cb(&a, th_set_y);
        lv_anim_set_values(&a, -20, TH_SCREEN_H + 10);
        lv_anim_set_time(&a, lv_rand(2500, 4500));
        lv_anim_set_delay(&a, lv_rand(0, 3000));
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
}

/* A score grew: the number pops in (moves down into place, 200 ms) and "+N" floats up from the right of the ring. */
static void gain_ready(lv_anim_t *a) { vis((lv_obj_t *)a->var, false); }

static void score_gain(int i, unsigned n)
{
    lv_obj_t *s = s_score[i], *g = s_gain[i];
    lv_anim_del(s, th_set_y);
    lv_anim_del(s, th_set_text_opa);
    th_anim(s, th_set_y, kRingY + kScoreDy - 14, kRingY + kScoreDy, kPopMs, false, lv_anim_path_overshoot);
    th_anim(s, th_set_text_opa, 90, 255, kPopMs, false, lv_anim_path_ease_out);

    char b[8];
    snprintf(b, sizeof(b), "+%u", n);
    lv_anim_del(g, th_set_y);
    lv_anim_del(g, th_set_text_opa);
    txt(g, b);
    lv_obj_set_style_text_opa(g, LV_OPA_COVER, 0);
    vis(g, true);
    th_anim(g, th_set_y, kRingY + kScoreDy, kRingY + kScoreDy - kGainRise, kGainMs, false, lv_anim_path_ease_out);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g);
    lv_anim_set_exec_cb(&a, th_set_text_opa);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_time(&a, kGainMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, gain_ready);
    lv_anim_start(&a);
}

static void bonus_banner(bool on)
{
    if (on == s_bonus_on) return;
    s_bonus_on = on;
    lv_anim_del(s_bonus, set_x);
    lv_anim_del(s_bonus, th_set_opa);
    lv_obj_set_style_opa(s_bonus, LV_OPA_COVER, 0);
    lv_obj_set_x(s_bonus, kBonusX);
    vis(s_bonus, on);
    if (on) {
        th_anim(s_bonus, set_x, -lv_obj_get_width(s_bonus), kBonusX, 300, false, lv_anim_path_ease_out);
        th_anim(s_bonus, th_set_opa, 255, 130, 800, true); /* slow breathing, like the idle pill */
    }
}

static void countdown_anim(uint8_t n)
{
    char b[8];
    snprintf(b, sizeof(b), "%u", (unsigned)n);
    lv_label_set_text(ui_labelCountDown, b);
    lv_anim_del(ui_labelCountDown, th_set_y);
    lv_anim_del(ui_labelCountDown, th_set_text_opa);
    th_anim(ui_labelCountDown, th_set_y, kCountdownY + 12, kCountdownY, 200, false, lv_anim_path_ease_out);
    th_anim(ui_labelCountDown, th_set_text_opa, 0, 255, 200, false, lv_anim_path_ease_out);
}

/* The winner is shown (round scores or final): only the winner keeps colour, every other player turns grey. */
static bool winner_shown(const StatePacket &st)
{
    return (st.phase == PH_FINAL || (st.phase == PH_RESULTS && (st.flags & STF_SCORES))) && st.winner >= 1 &&
           st.winner <= RTD_MAX_PLAYERS;
}

/* ---- centre area ---- */

static void deuce_timer_cb(lv_timer_t *)
{
    s_deuce_timer = NULL; /* one-shot, deleted by LVGL */
    s_deuce_done = true;
    screen_game_update(s_last, true);
}

static void apply_center(const StatePacket &st)
{
    uint8_t ph = st.phase;
    bool shake = st.mode == MODE_SHAKE;
    bool announce_deuce = false;

    if (ph == PH_ANNOUNCE && (st.flags & STF_DEUCE) && !s_deuce_done) {
        announce_deuce = true;
        if (!s_deuce_timer) {
            s_deuce_timer = lv_timer_create(deuce_timer_cb, 1500, NULL);
            lv_timer_set_repeat_count(s_deuce_timer, 1);
        }
    }

    bool circle = ph == PH_JOIN || ph == PH_ANNOUNCE || ph == PH_COUNTDOWN || ph == PH_REACT_WAIT ||
                  ph == PH_GO || ph == PH_SHAKE;
    bool show_win = winner_shown(st);

    vis(ui_centerCircle, circle);
    vis(s_pulse, ph == PH_REACT_WAIT);
    vis(ui_imgStart, false);
    vis(ui_imgPressJoyn, ph == PH_JOIN);
    vis(ui_imgDeuce, announce_deuce);
    vis(ui_imgReactMode, !announce_deuce && !shake && (ph == PH_ANNOUNCE || ph == PH_REACT_WAIT));
    vis(ui_imgShakeMode, (!announce_deuce && shake && ph == PH_ANNOUNCE) || ph == PH_SHAKE);
    vis(ui_shakeNumber, (!announce_deuce && shake && ph == PH_ANNOUNCE) || ph == PH_SHAKE);
    vis(ui_imgGo, ph == PH_GO);
    vis(ui_labelCountDown, ph == PH_COUNTDOWN);
    vis(ui_imgWinner, show_win);
    vis(s_winbox, show_win);
    vis(ui_labelWinnerNum, show_win);
    vis(s_final, ph == PH_FINAL);
    blink(s_newrec, ph == PH_RESULTS && (st.flags & STF_NEW_RECORD));
    confetti(ph == PH_FINAL);

    if (st.shake_target || shake) {
        char b[8];
        snprintf(b, sizeof(b), "%u", (unsigned)st.shake_target);
        txt(ui_shakeNumber, b);
    }
    if (show_win) {
        char b[16];
        snprintf(b, sizeof(b), "PLAYER %u", (unsigned)st.winner);
        txt(ui_labelWinnerNum, b);
        lv_color_t wc = th_stick(st.players[st.winner - 1].stick_id);
        if (lv_obj_get_style_border_color(s_winbox, LV_PART_MAIN).full != wc.full) {
            lv_obj_set_style_border_color(s_winbox, wc, 0);
        }
    }
}

/* ---- players ---- */

/* The score block replaces the V1 "PLAYER n" image from the first announce to the final. */
static bool scores_shown(const StatePacket &st)
{
    return st.phase >= PH_ANNOUNCE && st.phase <= PH_FINAL && st.round >= 1;
}

static void apply_player(int i, const StatePacket &st)
{
    const PlayerState &p = st.players[i];
    uint8_t ph = st.phase;
    bool joined = p.stick_id != 0;
    bool newly = joined && !((s_prev.joined >> i) & 1) && s_prev.valid;
    bool in_round = (st.active_mask >> i) & 1;
    bool prompted = ph == PH_JOIN && !joined && st.prompt_slot == i + 1;
    bool loser = winner_shown(st) && st.winner != i + 1; /* grey while the winner is shown */

    /* ring colour */
    lv_color_t ring = th_color(TH_BG);
    if (loser) {
        ring = th_color(kRingOff);
    } else if (joined) {
        ring = th_stick(p.stick_id);
    } else if (ph == PH_JOIN) {
        ring = th_color(prompted ? TH_GREEN : TH_RED);
    } else if (scores_shown(st)) {
        ring = th_color(kRingOff); /* all four rings look the same while the scores show */
    }
    lv_obj_set_style_border_color(s_panel[i], ring, 0);
    if (newly && ph == PH_JOIN) {
        lv_anim_del(s_panel[i], th_set_border_w);
        th_anim(s_panel[i], th_set_border_w, 12, 4, 250, false, lv_anim_path_ease_out);
    }
    ring_pulse(i, prompted, s_prev.prompted[i], 90);

    bool dim = !joined || (ph != PH_JOIN && ph != PH_IDLE && ph != PH_FINAL && !in_round);
    lv_obj_set_style_img_opa(s_pimg[i], dim ? LV_OPA_30 : LV_OPA_COVER, 0);
    tcol(s_name[i], (dim || loser) ? TH_TEXT2 : TH_TEXT);

    /* result text */
    char b[40];
    b[0] = 0;
    bool scores = (st.flags & STF_SCORES) != 0;
    /* The time replaces the score inside the ring from the press until the winner is shown (apply_scores). */
    bool result_phase = ph == PH_GO || ph == PH_SHAKE || (ph == PH_RESULTS && !scores);
    uint32_t col = TH_TEXT;
    bool ms = false;
    if (result_phase && p.status == PS_DONE && p.time_ms != TIME_NONE) {
        snprintf(b, sizeof(b), "%u", (unsigned)p.time_ms);
        ms = true;
    } else if (result_phase && p.status == PS_PENALTY) {
        snprintf(b, sizeof(b), "FOUL");
        col = TH_RED;
    }
    lv_obj_set_style_text_color(s_time[i], th_color(col), 0);
    txt(s_time[i], b);
    vis(s_time[i], b[0] != 0);
    vis(s_unit[i], ms);

    /* shake progress arc */
    bool arc_on = ph == PH_SHAKE && in_round && p.status == PS_PLAYING;
    vis(s_arc[i], arc_on);
    if (arc_on) {
        lv_obj_set_style_arc_color(s_arc[i], th_stick(p.stick_id), LV_PART_INDICATOR);
        if (lv_arc_get_value(s_arc[i]) != p.progress) lv_arc_set_value(s_arc[i], p.progress);
    }

    /* winner ring pulses in the results and in the final */
    bool win = (ph == PH_FINAL || (ph == PH_RESULTS && scores)) && st.winner == i + 1;
    ring_pulse(i, win, s_prev.winpulse[i], 100);
}

/* Score, leader tag and gain animation of every player. Players who did not join show a dash. */
static void apply_scores(const StatePacket &st)
{
    uint8_t ph = st.phase;
    bool show = scores_shown(st);
    bool results = ph == PH_RESULTS && (st.flags & STF_SCORES);
    bool win_show = winner_shown(st);
    uint8_t top = 0;
    for (int i = 0; i < RTD_MAX_PLAYERS; i++) {
        if (st.players[i].stick_id && st.players[i].score > top) top = st.players[i].score;
    }

    for (int i = 0; i < RTD_MAX_PLAYERS; i++) {
        const PlayerState &p = st.players[i];
        bool joined = p.stick_id != 0;
        bool on = show && joined;
        /* while the winner is shown only the winner is highlighted: no leader tag */
        bool lead = on && !win_show && top > 0 && p.score == top;
        bool timed = !lv_obj_has_flag(s_time[i], LV_OBJ_FLAG_HIDDEN); /* the time holds the score place */
        vis(s_pimg[i], !show);
        vis(s_name[i], show);
        vis(s_dash[i], show && !joined);
        vis(s_score[i], on && !timed);
        vis(s_rank[i], lead && !timed); /* only the leader (or the tied leaders) gets the tag */
        if (!on) continue;

        char b[8];
        snprintf(b, sizeof(b), "%u", (unsigned)p.score);
        bool grew = s_prev.valid && results && p.score > s_prev.score[i];
        tfont(s_score[i], p.score > 99 ? TH_FONT40 : TH_FONT64);
        tcol(s_score[i], (win_show && st.winner != i + 1) ? TH_TEXT2 : TH_TEXT);
        txt(s_score[i], b);
        if (grew) score_gain(i, (unsigned)(p.score - s_prev.score[i]));
    }
}

/* ---- update ---- */

void screen_game_update(const StatePacket &st, bool link_up)
{
    s_last = st;
    bool changed = !s_prev.valid || st.phase != s_prev.phase;

    if (st.phase != PH_ANNOUNCE && s_deuce_timer) {
        lv_timer_del(s_deuce_timer);
        s_deuce_timer = NULL;
    }
    if (!(st.flags & STF_DEUCE)) s_deuce_done = false;

    apply_center(st);
    for (int i = 0; i < RTD_MAX_PLAYERS; i++) apply_player(i, st);
    apply_scores(st);

    if (st.phase == PH_COUNTDOWN && (changed || st.countdown != s_prev.countdown) && st.countdown) {
        countdown_anim(st.countdown);
    }
    if (changed && (st.phase == PH_GO || st.phase == PH_SHAKE) && s_prev.valid) flash_go();

    char b[24];
    bool show_round = st.phase >= PH_ANNOUNCE && st.phase <= PH_FINAL && st.round >= 1;
    bool bonus = show_round && (st.flags & STF_BONUS) && !(st.flags & STF_DEUCE);
    if (st.flags & STF_DEUCE) {
        snprintf(b, sizeof(b), "DEUCE");
    } else {
        snprintf(b, sizeof(b), "ROUND %u/%u%s", (unsigned)st.round, (unsigned)st.total_rounds, bonus ? "  x2" : "");
    }
    txt(s_round, b);
    vis(s_round, show_round);
    bonus_banner(bonus);

    ui_chrome_update(s_chrome, link_up, false);

    s_prev.valid = true;
    s_prev.phase = st.phase;
    s_prev.countdown = st.countdown;
    s_prev.flags = st.flags;
    s_prev.winner = st.winner;
    s_prev.joined = 0;
    for (int i = 0; i < RTD_MAX_PLAYERS; i++) {
        if (st.players[i].stick_id) s_prev.joined |= (uint8_t)(1 << i);
        s_prev.status[i] = st.players[i].status;
        s_prev.score[i] = st.players[i].score;
    }
}

/* Leaving the screen: stop every running animation and forget the applied state. */
static void on_unloaded(lv_event_t *)
{
    for (int i = 0; i < RTD_MAX_PLAYERS; i++) {
        lv_anim_del(s_panel[i], th_set_border_opa);
        lv_anim_del(s_panel[i], th_set_border_w);
    }
    lv_anim_del(s_flash, th_set_bg_opa);
    vis(s_flash, false);
    lv_anim_del(s_newrec, th_set_opa);
    vis(s_newrec, false);
    for (int i = 0; i < RTD_MAX_PLAYERS; i++) {
        lv_anim_del(s_score[i], th_set_y);
        lv_anim_del(s_score[i], th_set_text_opa);
        lv_obj_set_y(s_score[i], kRingY + kScoreDy);
        lv_obj_set_style_text_opa(s_score[i], LV_OPA_COVER, 0);
        lv_anim_del(s_gain[i], th_set_y);
        lv_anim_del(s_gain[i], th_set_text_opa);
        vis(s_gain[i], false);
    }
    bonus_banner(false);
    confetti(false);
    if (s_deuce_timer) {
        lv_timer_del(s_deuce_timer);
        s_deuce_timer = NULL;
    }
    memset(&s_prev, 0, sizeof(s_prev));
    s_deuce_done = false;
}

static void pulse_ring_anim(lv_event_t *)
{
    /* the pulse ring only runs while visible: restarted by apply_center via vis() */
}

lv_obj_t *screen_game_init()
{
    ui_MainScreen_screen_init();
    lv_obj_t *scr = ui_MainScreen;
    lv_obj_set_style_bg_color(scr, th_color(TH_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *panels[RTD_MAX_PLAYERS] = { ui_Panel1, ui_Panel2, ui_Panel3, ui_Panel4 };
    lv_obj_t *pimgs[RTD_MAX_PLAYERS] = { ui_player1, ui_player2, ui_player3, ui_player4 };
    lv_obj_t *times[RTD_MAX_PLAYERS] = { ui_labelPlayer1Timing, ui_labelPlayer2Timing, ui_labelPlayer3Timing,
                                         ui_labelPlayer4Timing };
    for (int i = 0; i < RTD_MAX_PLAYERS; i++) {
        s_panel[i] = panels[i];
        s_pimg[i] = pimgs[i];
        s_time[i] = times[i];
        /* The time sits inside the ring in place of the score (below the ring it crossed the GO circle):
         * the number in Bold 40, "ms" in Bold 20 below it. */
        lv_obj_set_style_text_font(s_time[i], TH_FONT40, 0);
        lv_obj_set_style_text_align(s_time[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_time[i], LV_ALIGN_CENTER, kRingX[i], kRingY + kTimeDy);
        s_unit[i] = th_label(scr, TH_FONT20, TH_TEXT2, "ms");
        lv_obj_align(s_unit[i], LV_ALIGN_CENTER, kRingX[i], kRingY + kUnitDy);
        lv_obj_add_flag(s_unit[i], LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *a = lv_arc_create(scr);
        lv_obj_set_size(a, 166, 166);
        lv_obj_align(a, LV_ALIGN_CENTER, kRingX[i], kRingY);
        lv_arc_set_rotation(a, 270);
        lv_arc_set_bg_angles(a, 0, 360);
        lv_arc_set_range(a, 0, 100);
        lv_arc_set_value(a, 0);
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(a, 6, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_width(a, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(a, false, LV_PART_INDICATOR);
        lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
        s_arc[i] = a;

        /* while the scores show: "PLAYER n" text replaces the V1 image (apply_scores) */
        char nb[12];
        snprintf(nb, sizeof(nb), "PLAYER %d", i + 1);
        s_name[i] = th_label(scr, TH_FONT20, TH_TEXT, nb);
        lv_obj_align(s_name[i], LV_ALIGN_CENTER, kRingX[i], kRingY + kNameDy);
        lv_obj_add_flag(s_name[i], LV_OBJ_FLAG_HIDDEN);
        s_dash[i] = th_label(scr, TH_FONT40, kRingOff, "-");
        lv_obj_align(s_dash[i], LV_ALIGN_CENTER, kRingX[i], kRingY + kScoreDy);
        lv_obj_add_flag(s_dash[i], LV_OBJ_FLAG_HIDDEN);

        s_score[i] = th_label(scr, TH_FONT64, TH_TEXT, "0");
        lv_obj_align(s_score[i], LV_ALIGN_CENTER, kRingX[i], kRingY + kScoreDy);
        lv_obj_add_flag(s_score[i], LV_OBJ_FLAG_HIDDEN);
        s_rank[i] = th_label(scr, TH_FONT20, TH_BG, "1ST");
        lv_obj_align(s_rank[i], LV_ALIGN_CENTER, kRingX[i], kRingY + kRankDy);
        lv_obj_set_style_bg_color(s_rank[i], th_color(TH_ORANGE), 0);
        lv_obj_set_style_bg_opa(s_rank[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_rank[i], 13, 0);
        lv_obj_set_style_pad_hor(s_rank[i], 8, 0);
        lv_obj_add_flag(s_rank[i], LV_OBJ_FLAG_HIDDEN);
        s_gain[i] = th_label(scr, TH_FONT40, TH_ORANGE, "+1");
        lv_obj_align(s_gain[i], LV_ALIGN_CENTER, kRingX[i] + kGainDx, kRingY + kScoreDy);
        lv_obj_add_flag(s_gain[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* PH_REACT_WAIT: slow pulse of a thin ring around the centre circle, independent of the GO time */
    s_pulse = th_box(scr, 332, 332);
    lv_obj_align(s_pulse, LV_ALIGN_CENTER, 12, 72);
    lv_obj_set_style_radius(s_pulse, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_pulse, 4, 0);
    lv_obj_set_style_border_color(s_pulse, th_color(TH_GREEN), 0);
    lv_obj_add_flag(s_pulse, LV_OBJ_FLAG_HIDDEN);
    th_anim(s_pulse, th_set_border_opa, 255, 40, 1000, true);

    s_round = th_label(scr, TH_FONT20, TH_TEXT2, "");
    lv_obj_align(s_round, LV_ALIGN_BOTTOM_LEFT, 24, -22);
    lv_obj_add_flag(s_round, LV_OBJ_FLAG_HIDDEN);

    s_newrec = th_label(scr, TH_FONT28, TH_ORANGE, "NEW RECORD!");
    lv_obj_align(s_newrec, LV_ALIGN_CENTER, 0, -2);
    lv_obj_add_flag(s_newrec, LV_OBJ_FLAG_HIDDEN);

    /* bonus round banner: orange pill, the text wraps into "BONUS ROUND" / "x2" at the width of the first line */
    lv_coord_t tw = lv_txt_get_width("BONUS ROUND", 11, TH_FONT28, 0, LV_TEXT_FLAG_NONE) + 20;
    s_bonus = th_box(scr, tw + 2 * kBonusPad, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_bonus, 20, 0);
    lv_obj_set_style_bg_color(s_bonus, th_color(TH_ORANGE), 0);
    lv_obj_set_style_bg_opa(s_bonus, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_bonus, kBonusPad, 0);
    lv_obj_set_pos(s_bonus, kBonusX, kBonusY);
    lv_obj_t *bt = th_label(s_bonus, TH_FONT28, TH_BG, "BONUS ROUND  x2");
    lv_obj_set_width(bt, tw);
    lv_obj_add_flag(s_bonus, LV_OBJ_FLAG_HIDDEN);

    /* Winner: the V1 box with "WINNER" centred in it (the V1 image had the word 6 px left of centre and "PLAYER"
     * drawn in), same size and top edge as V1. The image holds only the word; the frame (V1 geometry: 423x116,
     * 6 px, inside the 431x119 image) is drawn here in the colour of the winner's stick (apply_center).
     * "PLAYER n" below is one centred label, so any digit lines up. */
    lv_img_set_src(ui_imgWinner, &ui_img_winner_text);
    lv_obj_align(ui_imgWinner, LV_ALIGN_CENTER, 1, 62);
    s_winbox = th_box(scr, 423, 116);
    lv_obj_align(s_winbox, LV_ALIGN_CENTER, 1, 63);
    lv_obj_set_style_radius(s_winbox, 18, 0);
    lv_obj_set_style_border_width(s_winbox, 6, 0);
    lv_obj_set_style_border_color(s_winbox, th_color(TH_GREEN_WIN), 0);
    lv_obj_clear_flag(s_winbox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_winbox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(ui_labelWinnerNum, LV_ALIGN_CENTER, 0, 176);
    lv_label_set_text(ui_labelWinnerNum, "PLAYER 1");

    s_final = th_label(scr, TH_FONT28, TH_TEXT, "FINAL WINNER");
    lv_obj_align(s_final, LV_ALIGN_CENTER, 0, -2);
    lv_obj_add_flag(s_final, LV_OBJ_FLAG_HIDDEN);

    ui_chrome_create(scr, s_chrome, LV_OPA_40);

    s_flash = th_box(scr, TH_SCREEN_W, TH_SCREEN_H);
    lv_obj_set_style_bg_color(s_flash, th_color(TH_ORANGE), 0);
    lv_obj_set_style_bg_opa(s_flash, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_flash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_flash, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(scr, on_unloaded, LV_EVENT_SCREEN_UNLOADED, NULL);
    (void)pulse_ring_anim;
    return scr;
}
