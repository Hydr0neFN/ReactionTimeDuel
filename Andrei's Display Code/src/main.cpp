#include "lvgl.h"
#include "display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "ui.h"

void taskLVGL(void*) {
    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    
}

extern "C" void app_main(void) {
    ESP_LOGI("MEM", "PSRAM size: %d", (int)esp_psram_get_size());
    lv_init();
    display_init();
    ui_init();

    xTaskCreatePinnedToCore(taskLVGL, "taskLVGL",
                        16 * 1024,
                        NULL, 5, NULL, 1);
}
