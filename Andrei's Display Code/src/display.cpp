#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t* s_buf1 = nullptr;
static lv_color_t* s_buf2 = nullptr;
static lv_disp_drv_t s_disp_drv;  // optional, but helps keep things stable

static LGFX_CONF lcd;

static void lvgl_flush_cb(lv_disp_drv_t *disp,
                          const lv_area_t *area,
                          lv_color_t *color_p)
{
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;

  lcd.startWrite();
  lcd.setAddrWindow(area->x1, area->y1, w, h);
  lcd.pushPixels((uint16_t *)color_p, w * h);
  lcd.endWrite();

  lv_disp_flush_ready(disp);
}

void display_init(void)
{
  ESP_LOGI("MEM", "Free INTERNAL: %u",
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  ESP_LOGI("MEM", "Free DMA     : %u",
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
  ESP_LOGI("MEM", "Free SPIRAM  : %u",
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  lcd.init();
  lcd.setSwapBytes(true);
  lcd.fillScreen(TFT_BLACK);
  lcd.setBrightness(100);

  constexpr int BUF_LINES = 10;  // start small

  s_buf1 = (lv_color_t*) heap_caps_malloc(800 * BUF_LINES * sizeof(lv_color_t),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  s_buf2 = (lv_color_t*) heap_caps_malloc(800 * BUF_LINES * sizeof(lv_color_t),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

  assert(s_buf1 && s_buf2);

  // Use the same size as the allocated buffers.
  lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, 800 * BUF_LINES);

  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = 800;
  s_disp_drv.ver_res = 480;
  s_disp_drv.flush_cb = lvgl_flush_cb;
  s_disp_drv.draw_buf = &s_draw_buf;

  lv_disp_drv_register(&s_disp_drv);
}
