#ifndef RTD_BOARD_H
#define RTD_BOARD_H

/*
 * Host board (ESP32 DevKit-C) pins and LED geometry. Baseline hardware,
 * unchanged by DESIGN.md.
 *
 *   GPIO4:  NeoPixel DIN (5 rings x 12 LEDs = 60)
 *   GPIO16: WS2812B strip DIN (89 LEDs, ambient animations)
 *   GPIO25: I2S DOUT
 *   GPIO26: I2S BCLK
 *   GPIO27: I2S LRC
 *   GPIO33: amplifier GAIN (LOW = maximum volume)
 *   GPIO0:  BOOT button, held 3 s toggles maintenance mode (DESIGN.md 9.1)
 */

#define PIN_NEOPIXEL      4
#define PIN_STRIP         16
#define PIN_BOOT          0

#define PIN_I2S_DOUT      25
#define PIN_I2S_BCLK      26
#define PIN_I2S_LRC       27
#define PIN_AMP_GAIN      33

#define NEOPIXEL_COUNT    60
#define LEDS_PER_RING     12
#define NUM_RINGS         5
#define CENTER_RING       2
#define STRIP_LED_COUNT   89

#endif /* RTD_BOARD_H */
