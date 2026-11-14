#pragma once

/*
 * Theme tokens of DESIGN.md 7.3 and small widget helpers shared by all screens.
 * Text must stay inside ASCII 0x20..0x7F: the Bold fonts hold nothing else.
 */

#include "lvgl.h"
#include "ui.h"

#define TH_BG            0x101418u
#define TH_TEXT          0xFFFFFFu
#define TH_ORANGE        0xFF9430u
#define TH_GREEN         0x2AAC00u   /* ring */
#define TH_GREEN_WIN     0x40CC00u   /* winner box */
#define TH_RED           0xE81818u   /* danger, free slot */
#define TH_TEXT2         0x8A9099u   /* secondary text */
#define TH_PANEL         0x1A2026u
#define TH_PANEL_RADIUS  12

#define TH_SCREEN_W      800
#define TH_SCREEN_H      480

#define TH_FONT20        (&ui_font_MontserratBold20)
#define TH_FONT28        (&ui_font_MontserratBold28)
#define TH_FONT40        (&ui_font_MontserratBold40)
#define TH_FONT64        (&ui_font_MonserratBold64V2)
#define TH_FONT120       (&ui_font_MontserratBold120)
/* LV_SYMBOL_* icons only */
#define TH_FONT_SYMBOL   (&lv_font_montserrat_24)

/* Stick colours indexed by stick ID 1..4; index 0 = empty slot (background). */
static const uint32_t TH_STICK[5] = { TH_BG, 0xFFFFFFu, 0x0000FFu, 0xFF0000u, 0xFFFF00u };

static inline lv_color_t th_color(uint32_t hex) { return lv_color_hex(hex); }

static inline lv_color_t th_stick(uint8_t stick_id)
{
    return lv_color_hex(TH_STICK[stick_id <= 4 ? stick_id : 0]);
}

/* Label with font and colour, no wrapping. */
static inline lv_obj_t *th_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, text);
    return l;
}

/* Plain container: no border, no padding, no scrolling, transparent. */
static inline lv_obj_t *th_box(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* Rounded panel (fill 0x1A2026, radius 12). */
static inline lv_obj_t *th_panel(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *o = th_box(parent, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(TH_PANEL), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, TH_PANEL_RADIUS, 0);
    return o;
}

/* Screen with the dark background, no scrolling. */
static inline lv_obj_t *th_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    return s;
}

/* One-shot / looping animation helpers. */
typedef void (*th_exec_t)(void *, int32_t);

static inline void th_anim(lv_obj_t *obj, th_exec_t cb, int32_t from, int32_t to, uint32_t ms,
                           bool loop_pingpong, lv_anim_path_cb_t path = lv_anim_path_ease_in_out)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, path);
    if (loop_pingpong) {
        lv_anim_set_playback_time(&a, ms);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    }
    lv_anim_start(&a);
}

static inline void th_set_y(void *o, int32_t v) { lv_obj_set_y((lv_obj_t *)o, (lv_coord_t)v); }
static inline void th_set_opa(void *o, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }
static inline void th_set_border_opa(void *o, int32_t v)
{
    lv_obj_set_style_border_opa((lv_obj_t *)o, (lv_opa_t)v, 0);
}
static inline void th_set_border_w(void *o, int32_t v)
{
    lv_obj_set_style_border_width((lv_obj_t *)o, (lv_coord_t)v, 0);
}
static inline void th_set_bg_opa(void *o, int32_t v) { lv_obj_set_style_bg_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }
static inline void th_set_text_opa(void *o, int32_t v) { lv_obj_set_style_text_opa((lv_obj_t *)o, (lv_opa_t)v, 0); }
