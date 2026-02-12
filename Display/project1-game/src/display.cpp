#include "display.h"
#include "waveshare_rgb_lcd_port.h"

void display_init(void) {
  waveshare_esp32_s3_rgb_lcd_init();
}
