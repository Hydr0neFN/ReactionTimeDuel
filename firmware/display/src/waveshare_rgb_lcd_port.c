/*
 * RGB LCD init based on Waveshare ESP-IDF demo (touch disabled).
 */

#include "waveshare_rgb_lcd_port.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "lvgl_port.h"
#include <string.h>

static const char *TAG = "waveshare_rgb";

// Panel config (Waveshare defaults)
#define LCD_H_RES               (LVGL_PORT_H_RES)
#define LCD_V_RES               (LVGL_PORT_V_RES)
#define LCD_PIXEL_CLOCK_HZ      (16 * 1000 * 1000)
#define RGB_DATA_WIDTH          (16)
#define RGB_BPP                 (16)
#define RGB_BOUNCE_HEIGHT       (10)
#define RGB_BOUNCE_BUFFER_SIZE  (LCD_H_RES * RGB_BOUNCE_HEIGHT)

#define LCD_IO_VSYNC        (GPIO_NUM_3)
#define LCD_IO_HSYNC        (GPIO_NUM_46)
#define LCD_IO_DE           (GPIO_NUM_5)
#define LCD_IO_PCLK         (GPIO_NUM_7)
#define LCD_IO_DISP         (-1)

#define LCD_IO_DATA0        (GPIO_NUM_14)
#define LCD_IO_DATA1        (GPIO_NUM_38)
#define LCD_IO_DATA2        (GPIO_NUM_18)
#define LCD_IO_DATA3        (GPIO_NUM_17)
#define LCD_IO_DATA4        (GPIO_NUM_10)
#define LCD_IO_DATA5        (GPIO_NUM_39)
#define LCD_IO_DATA6        (GPIO_NUM_0)
#define LCD_IO_DATA7        (GPIO_NUM_45)
#define LCD_IO_DATA8        (GPIO_NUM_48)
#define LCD_IO_DATA9        (GPIO_NUM_47)
#define LCD_IO_DATA10       (GPIO_NUM_21)
#define LCD_IO_DATA11       (GPIO_NUM_1)
#define LCD_IO_DATA12       (GPIO_NUM_2)
#define LCD_IO_DATA13       (GPIO_NUM_42)
#define LCD_IO_DATA14       (GPIO_NUM_41)
#define LCD_IO_DATA15       (GPIO_NUM_40)

IRAM_ATTR static bool rgb_lcd_on_vsync_event(esp_lcd_panel_handle_t panel,
                                            const esp_lcd_rgb_panel_event_data_t *edata,
                                            void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;
    return lvgl_port_notify_rgb_vsync();
}

esp_err_t waveshare_esp32_s3_rgb_lcd_init(void)
{
    ESP_LOGI(TAG, "Install RGB LCD panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings =  {
            .pclk_hz = LCD_PIXEL_CLOCK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags = {
                .pclk_active_neg = 1,
            },
        },
        .data_width = RGB_DATA_WIDTH,
        .bits_per_pixel = RGB_BPP,
        .num_fbs = LVGL_PORT_LCD_RGB_BUFFER_NUMS,
        .bounce_buffer_size_px = RGB_BOUNCE_BUFFER_SIZE,
        .sram_trans_align = 4,
        .psram_trans_align = 64,
        .hsync_gpio_num = LCD_IO_HSYNC,
        .vsync_gpio_num = LCD_IO_VSYNC,
        .de_gpio_num = LCD_IO_DE,
        .pclk_gpio_num = LCD_IO_PCLK,
        .disp_gpio_num = LCD_IO_DISP,
        .data_gpio_nums = {
            LCD_IO_DATA0,
            LCD_IO_DATA1,
            LCD_IO_DATA2,
            LCD_IO_DATA3,
            LCD_IO_DATA4,
            LCD_IO_DATA5,
            LCD_IO_DATA6,
            LCD_IO_DATA7,
            LCD_IO_DATA8,
            LCD_IO_DATA9,
            LCD_IO_DATA10,
            LCD_IO_DATA11,
            LCD_IO_DATA12,
            LCD_IO_DATA13,
            LCD_IO_DATA14,
            LCD_IO_DATA15,
        },
        .flags = {
            .fb_in_psram = 1,
        },
    };

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    // Clear frame buffers to avoid startup garbage/flicker
    void *fbs[3] = {0};
    const size_t fb_bytes = LCD_H_RES * LCD_V_RES * (RGB_BPP / 8);
    if (LVGL_PORT_LCD_RGB_BUFFER_NUMS == 3) {
        ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 3, &fbs[0], &fbs[1], &fbs[2]));
    } else if (LVGL_PORT_LCD_RGB_BUFFER_NUMS == 2) {
        ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &fbs[0], &fbs[1]));
    } else if (LVGL_PORT_LCD_RGB_BUFFER_NUMS == 1) {
        ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 1, &fbs[0]));
    }
    for (size_t i = 0; i < 3; i++) {
        if (fbs[i]) {
            memset(fbs[i], 0x00, fb_bytes);
        }
    }

    ESP_ERROR_CHECK(lvgl_port_init(panel_handle, NULL));

    // Register VSYNC callback (after LVGL task is ready)
    esp_lcd_rgb_panel_event_callbacks_t cbs = { 0 };
#if RGB_BOUNCE_BUFFER_SIZE > 0
    cbs.on_bounce_frame_finish = rgb_lcd_on_vsync_event;
#else
    cbs.on_vsync = rgb_lcd_on_vsync_event;
#endif
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, NULL));

    return ESP_OK;
}
