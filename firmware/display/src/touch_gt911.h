#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reset the GT911 through the CH422G expander and probe it (DESIGN.md 7.2).
 * Returns false when the controller does not answer; the caller then runs without touch. */
bool touch_gt911_init(void);

/* Read the first touch point. Returns true while a finger is down; x and y are in 800x480 pixels. */
bool touch_gt911_read(uint16_t *x, uint16_t *y);

/* Register the controller as an LVGL pointer input device (long press time 1000 ms).
 * Call with the LVGL lock held, after touch_gt911_init() returned true. */
void touch_gt911_register_lvgl(void);

#ifdef __cplusplus
}
#endif
