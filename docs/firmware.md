**English** · [繁體中文](firmware.zh-TW.md)

# Host and joystick firmware

The host (ESP32) and slave joysticks (ESP8266) share `Protocol.h` for the packet format and command definitions. Each has its own `GameTypes.h` tailored to its role.

## Host vs slave

| | Host (ESP32) | Slave (ESP8266) |
|---|---|---|
| **Role** | Game orchestrator, LED/audio output | Input device, sends results |
| **Clock** | 240 MHz | 160 MHz |
| **Protocol.h** | Includes display commands (`DISP_*`) | Core commands only |
| **NeoPixel modes** | 8 modes (rainbow, status, blink slot, shake countdown, etc.) | 6 modes (no blink slot / shake countdown) |
| **Ring mapping** | Reversed: P1=Ring4, P2=Ring3, P3=Ring1, P4=Ring0 | Sequential: P1=Ring0, P2=Ring1, center=Ring2, P3=Ring3, P4=Ring4 |
| **State machine** | 8 game states with full round management | 4 joystick states (idle, waiting GO, timing/shaking, done) |
| **Audio** | Non-blocking MP3 queue with I2S + PWM volume | No audio hardware |
| **Peripherals** | NeoPixel rings, WS2812B strip, I2S DAC, amplifier | Button, MPU-6050, vibration motor |

## Joystick state machine

Each joystick follows this per-round lifecycle:

```
JS_IDLE ──> JS_WAITING_GO ──> JS_REACTION_TIMING  ──> JS_DONE
                          └──> JS_SHAKE_COUNTING   ──> JS_DONE
```

- **JS_IDLE** — Debounced button poll; press sends `CMD_REQ_ID` with firmware version
- **JS_WAITING_GO** — Received `CMD_GAME_START`; waits for `CMD_GO`
- **JS_REACTION_TIMING** — `micros()` timer running; button interrupt captures timestamp
- **JS_SHAKE_COUNTING** — MPU-6050 high-pass filter counts full push-return cycles; reports progress every 5 shakes
- **JS_DONE** — Result sent; waits for next round or idle command
