/*
 * Reaction Time Duel 2.0 - display firmware (ESP32-S3, ESP-IDF, LVGL 8.3). DESIGN.md section 7 and 9.5.
 * Boot order: touch (also switches the backlight on) -> RGB panel + LVGL -> radio -> UI.
 */

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "lvgl_port.h"
#include "waveshare_rgb_lcd_port.h"

#include "DisplayLink.h"
#include "RtdProtocol.h"
#include "diag.h"
#include "net.h"
#include "touch_gt911.h"
#include "ui_app.h"

static const char *TAG = "DISPLAY";

extern "C" void app_main(void)
{
    /* Logged so the linker keeps the marker string that the OTA check of the host looks for. */
    ESP_LOGI(TAG, "%s %s", RTD_FW_MARKER(display), FW_VERSION_STRING);

    bool touch_ok = touch_gt911_init();
    if (!touch_ok) {
        ESP_LOGW(TAG, "running without touch");
    }

    ESP_ERROR_CHECK(waveshare_esp32_s3_rgb_lcd_init());
    net_init();
    diag_set_touch_ok(touch_ok);
    ESP_LOGI(TAG, "reset reason %d (%s), free internal heap %u bytes", (int)esp_reset_reason(),
             diag_reset_name((int)esp_reset_reason()), (unsigned)esp_get_free_internal_heap_size());

    static DisplayLink link(net_ports());
    if (lvgl_port_lock(-1)) {
        if (touch_ok) {
            touch_gt911_register_lvgl();
        }
        ui_app_init(link);
        lvgl_port_unlock();
    }
}
