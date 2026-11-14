#pragma once

#include "lvgl.h"
#include "RtdProtocol.h"

/* Settings screen: three tabs, opened by a long press on the gear (DESIGN.md 7.4). */
lv_obj_t *screen_settings_init();
void screen_settings_open();   /* back to the first tab, controls follow the snapshot */
void screen_settings_update(const StatePacket &st, bool link_up);

/* Confirmation box (YES / NO). YES sends the action to the host. Also used by the service screen. */
void ui_confirm(uint8_t act, uint8_t arg, const char *text);
