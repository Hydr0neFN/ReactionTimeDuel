#pragma once

#include "lvgl.h"
#include "RtdProtocol.h"

/*
 * Service screen (DESIGN.md 12): header with four page buttons and CLOSE, pages UPDATE, STICKS, HOST, DISPLAY.
 * Replaces the old maintenance screen; PH_MAINT always shows it (ui_app.cpp decides when).
 */
lv_obj_t *screen_maint_init();
void screen_maint_open();   /* the screen was just shown: UPDATE page, refresh at once */
void screen_maint_update(const StatePacket &st, bool link_up); /* every UI pass; refreshes at most every 500 ms */
