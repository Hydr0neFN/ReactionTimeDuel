#pragma once

#include "lvgl.h"
#include "RtdProtocol.h"

/* Game screen (all game phases): the SquareLine main screen plus the additions of DESIGN.md 7.4. */
lv_obj_t *screen_game_init();
void screen_game_update(const StatePacket &st, bool link_up);
