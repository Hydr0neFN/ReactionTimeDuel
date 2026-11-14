#pragma once

/*
 * UI shell: screens, link-lost overlay, snapshot dispatch (DESIGN.md 7.4, 7.5).
 * Everything here runs in the LVGL task (the 20 ms timer and the widget events).
 */

#include "lvgl.h"
#include "DisplayLink.h"
#include "RtdService.h"

/* Create all screens and the 20 ms timer. Call once with the LVGL lock held, after net_init(). */
void ui_app_init(DisplayLink &link);

DisplayLink &ui_link();

/* Settings screen: opened by a long press on the gear, closed by the back button or after 30 s idle. */
void ui_open_settings();
void ui_close_settings();

/* Service screen (screen_maint.*, DESIGN.md 12): opened from the settings STATUS tab while the host is idle, shown
 * always in PH_MAINT. Closing is refused in PH_MAINT (the screen is left with LEAVE UPDATE MODE). */
void ui_open_service();
void ui_close_service();

/* Last valid ServicePacket and its receive time (esp_timer ms). False until the first one arrived. */
bool ui_service(const ServicePacket **sp, uint32_t *rx_ms);

/* One CMD_SVC_REQ to the host (SVC_OP_*). */
void ui_send_svc(uint8_t op, uint8_t arg);

/* Link and gear icons (and the UPDATE AVAILABLE badge) at the bottom right of a screen. */
struct Chrome {
    lv_obj_t *link_icon;
    lv_obj_t *gear;
    lv_obj_t *badge;
};
void ui_chrome_create(lv_obj_t *screen, Chrome &c, lv_opa_t opa);
void ui_chrome_update(Chrome &c, bool link_up, bool update_available);
