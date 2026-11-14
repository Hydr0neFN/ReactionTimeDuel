**English** · [繁體中文](hardware.zh-TW.md)

# Hardware details

Per-unit hardware and pin assignments. The component summary is in the [README](../README.md).

## Host Controller (ESP32)

- **5 NeoPixel rings** (12 LEDs each, 60 total) — player status and game animations
- **89-LED WS2812B ambient strip** — cycles through 6 animations (rainbow, sparkle, meteor, color chase, breathing, fire)
- **I2S MP3 audio** — 24 sound files on SPIFFS for announcements, countdowns, and effects
- **PWM volume control** — hardware gain via amplifier GAIN pin

### Host pins

| Pin | Function |
|-----|----------|
| GPIO4 | NeoPixel ring data (5 rings, 60 LEDs) |
| GPIO16 | WS2812B ambient strip data (89 LEDs) |
| GPIO25 | I2S DOUT (audio data) |
| GPIO26 | I2S BCLK (bit clock) |
| GPIO27 | I2S LRC (word select) |
| GPIO33 | Amplifier GAIN (PWM volume control) |

## Joystick Controller (ESP8266)

- **Push button** (GPIO14, active LOW) — reaction input with IRAM interrupt for microsecond precision
- **MPU-6050 accelerometer** — shake detection with high-pass filtered hysteresis (X+Z axes)
- **Vibration motor** — haptic feedback on countdown ticks, GO signal, and completion

### Joystick pins

| Pin | Function |
|-----|----------|
| GPIO14 | Button input (active LOW, internal pullup) |
| GPIO12 | Vibration motor output |
| GPIO4 | I2C SDA (MPU-6050) |
| GPIO5 | I2C SCL (MPU-6050, 400kHz) |
