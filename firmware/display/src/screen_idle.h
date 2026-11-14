#pragma once

#include "lvgl.h"
#include "RtdProtocol.h"

/* Idle screen (PH_IDLE): logo, title, join pill, stats, stick dots, drifting circles. */
lv_obj_t *screen_idle_init();
void screen_idle_update(const StatePacket &st, bool link_up);
