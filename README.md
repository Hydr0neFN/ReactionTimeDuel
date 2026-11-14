**English** · [繁體中文](README.zh-TW.md)

# Reaction Time Duel

Selected for the Hanze Open Day showcase.

<img width="2048" height="1536" alt="IMG_2032" src="https://github.com/user-attachments/assets/0c61f458-73cb-4014-977f-d88b95b5550a" />

A competitive multiplayer reaction-time game on ESP32 and ESP8266 boards that talk over ESP-NOW. Up to 4 players compete across 5 rounds in two game modes, each holding a custom wireless joystick.

## At a glance

| | |
|---|---|
| What it is | Multiplayer reaction-time game on ESP32 + ESP8266, over ESP-NOW; up to 4 players, 5 rounds, 2 modes |
| Hardware | 1 ESP32 DevKit-C host, up to 4 ESP8266 ESP-12F joysticks, 1 optional display |
| Stack | Arduino / PlatformIO firmware, ESP-NOW, NeoPixelBus, ESP8266Audio, MPU-6050 |
| Status | Archived |
| My role | Team project (no individual role stated) |

## How it works

```
                         ┌──────────────────────┐
                         │     ESP32 Host        │
                         │    (Game Logic)       │
                         │                      │
                         │  NeoPixel Rings ×5   │
                         │  WS2812B Strip (89)  │
                         │  I2S Audio + Amp     │
                         │  SPIFFS (24 MP3s)    │
                         └──────────┬───────────┘
                                    │ ESP-NOW (ch 6)
              ┌─────────────────────┼─────────────────────┐
              │                     │                     │
        ┌─────┴──────┐       ┌─────┴──────┐        ┌─────┴──────┐
        │ Joystick 1 │       │ Joystick 2 │  ...   │ Joystick 4 │
        │  ESP8266   │       │  ESP8266   │        │  ESP8266   │
        │            │       │            │        │            │
        │  Button    │       │  Button    │        │  Button    │
        │  MPU-6050  │       │  MPU-6050  │        │  MPU-6050  │
        │  Vibration │       │  Vibration │        │  Vibration │
        └────────────┘       └────────────┘        └────────────┘
                                    │
                              ┌─────┴──────┐
                              │  Display   │
                              │ (optional) │
                              └────────────┘
```

- The ESP32 host runs the game logic. It drives 5 NeoPixel rings, an 89-LED WS2812B strip and the I2S audio.
- Up to 4 ESP8266 joysticks join over ESP-NOW on channel 6. Each sends its button or shake result back to the host.
- An optional display receives the same ESP-NOW status stream and shows the game state.
- The host picks the mode from a shuffle bag, runs the round and announces the winner.

## Game modes

### Reaction mode

The NeoPixel rings cycle through random colors. When the LEDs freeze to yellow, press your button. The fastest reaction time wins the round. Press too early and you get a penalty.

### Shake mode

After a 3-2-1 countdown, shake your joystick to reach the target count: 10, 15 or 20 shakes. A center ring countdown shows the remaining time. The first player to finish wins.

> Modes are distributed via a **shuffle bag**: both modes are guaranteed to appear before either repeats.

## Game flow

```
IDLE ──> JOIN ──> COUNTDOWN ──> REACTION / SHAKE ──> COLLECT ──> RESULTS ──┐
  ^                                                                        │
  │  (after 5 rounds)    FINAL_WINNER <────────────────────────────────────┘
  └──────────────────────────┘
```

1. **Idle** — Rainbow animation, "press to join" audio prompt.
2. **Join** — Players are prompted sequentially (P1 → P2 → P3 → P4). Each slot blinks on its NeoPixel ring for 5 seconds. Any unclaimed joystick can press to claim it. Minimum 2 players required.
3. **Mode selection** — Shuffle bag picks Reaction or Shake. First-time instructions play once per mode per game.
4. **Countdown** — Shake mode gets a 3-2-1 countdown with synced audio, NeoPixel flash and haptic vibration. Reaction mode skips the countdown and uses a random delay (3s, 5s, or 7s) before GO.
5. **Play** — Reaction: the LEDs freeze to yellow, press now. Shake: race to hit the target count.
6. **Collect** — Results arrive via ESP-NOW. A yellow warning phase runs for slow players before disqualification.
7. **Results** — Times are displayed for 3s, then the winner and scores for 3s.
8. **Final winner** — After 5 rounds, the winner is announced with a victory fanfare.

## Hardware

| Component | MCU | Qty | Role |
|-----------|-----|-----|------|
| Host Controller | ESP32 (DevKit-C) | 1 | Game logic, NeoPixel rings, ambient LED strip, I2S audio |
| Joystick | ESP8266 (ESP-12F) | up to 4 | Button input, accelerometer, vibration motor |
| Display | ESP32/ESP8266 | 1 | Game status display (optional, receives ESP-NOW) |

Pin assignments and the per-unit hardware lists are in [docs/hardware.md](docs/hardware.md) · [zh-TW](docs/hardware.zh-TW.md).

## Build / run

### Prerequisites

- [PlatformIO](https://platformio.org/) IDE or CLI
- ESP32 DevKit-C and ESP-12F boards
- USB cables for programming

### Host controller (ESP32)

```bash
cd ReactionTimerHost
pio run -t upload          # Flash firmware
pio run -t uploadfs        # Upload MP3 audio files to SPIFFS
```

### Joysticks (ESP8266)

Each joystick has a unique build environment with its own `MY_ID` flag:

```bash
cd ReactionTimerSlave
pio run -e stick1 -t upload    # Joystick 1 (MY_ID=0x01)
pio run -e stick2 -t upload    # Joystick 2 (MY_ID=0x02)
pio run -e stick3 -t upload    # Joystick 3 (MY_ID=0x03)
pio run -e stick4 -t upload    # Joystick 4 (MY_ID=0x04)
```

## Repo layout

```
ReactionTimeDuel/
├── README.md
├── .gitignore
│
├── ReactionTimerHost/              # ESP32 Host Controller
│   ├── platformio.ini
│   ├── enable_ccache.py            # Build speed optimization
│   ├── src/
│   │   └── main.cpp                # Game state machine, NeoPixel, strip animations
│   ├── include/
│   │   ├── Protocol.h              # Packet format, CRC8, device IDs, commands
│   │   ├── GameTypes.h             # Constants, timing, player struct, NeoPixel config
│   │   └── AudioManager.h          # Non-blocking MP3 queue, I2S output, PWM volume, sound defs
│   └── data/                       # 24 MP3 files uploaded to SPIFFS
│       ├── beep.mp3, click.mp3, error.mp3
│       ├── get_ready.mp3, react_inst.mp3, will_shake.mp3
│       ├── player1.mp3 ... player4.mp3
│       ├── one.mp3, two.mp3, three.mp3, ten.mp3, fifteen.mp3, twenty.mp3
│       ├── reaction.mp3, shake.mp3, fastest.mp3, wins.mp3
│       └── victory.mp3, gameover.mp3, press_join.mp3, ready.mp3
│
└── ReactionTimerSlave/             # ESP8266 Joystick Controllers (×4)
    ├── platformio.ini              # Multi-env: stick1, stick2, stick3, stick4
    ├── enable_ccache.py
    ├── src/
    │   └── main.cpp                # Joystick state machine, shake detection, timing
    └── include/
        ├── Protocol.h              # Shared protocol (core commands, no display cmds)
        └── GameTypes.h             # Joystick-only constants (timeouts)
```

## Design notes

- **Microsecond reaction timing** — The button press is captured by an `IRAM_ATTR` interrupt on the falling edge. The time is the `micros()` delta from the GO signal.
- **Non-blocking architecture** — NeoPixelBus with ESP32 RMT DMA gives glitch-free LED output. The audio queue has a configurable gap between sounds. There is no `delay()` in the game loop.
- **High-pass shake detection** — An EMA low-pass filter (alpha=1/64) removes gravity. A hysteresis state machine counts full push-return cycles on the X+Z axes.
- **Real-time shake progress** — Joysticks report a milestone every 5 shakes via `CMD_SHAKE_PROGRESS`, so the host can animate a live progress bar on its NeoPixel rings.
- **Shuffle bag mode selection** — Reaction and Shake both appear before either repeats, so there are no streaks.
- **Ambient light strip** — The 89-LED WS2812B strip runs 6 procedural animations (rainbow, sparkle, meteor rain, color chase, breathing, fire) on a second RMT channel.
- **PWM volume control** — The amplifier GAIN pin is driven by 25kHz LEDC PWM for smooth analog volume adjustment.
- **Firmware versioning** — Join packets carry the firmware version (V3.0.0) for a compatibility check.
- **Reliable delivery** — The host uses an ACK table with up to 3 retries (50ms interval) for critical commands.
- **Accessibility** — Full audio narration (24 MP3 files) covers all game states, player announcements and instructions.

## Protocol and firmware

The host and the slave joysticks share `Protocol.h`: a 7-byte packet over ESP-NOW on channel 6, with CRC8 (polynomial 0x8C) over bytes 0-5. Each side has its own `GameTypes.h` for its role.

- [Communication protocol](docs/protocol.md) · [zh-TW](docs/protocol.zh-TW.md) — packet fields, key commands and display commands.
- [Host and joystick firmware](docs/firmware.md) · [zh-TW](docs/firmware.zh-TW.md) — host/slave comparison and the joystick state machine.

## Dependencies

### Host (ESP32)

| Library | Version | Purpose |
|---------|---------|---------|
| [NeoPixelBus](https://github.com/Makuna/NeoPixelBus) | ≥2.8.3 | LED ring + strip control (RMT DMA) |
| [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio) | 1.9.7 | MP3 decoding + I2S output |

### Joystick (ESP8266)

| Library | Source | Purpose |
|---------|--------|---------|
| ESP8266WiFi | Built-in | ESP-NOW transport |
| Wire | Built-in | I2C for MPU-6050 |

## License

MIT License

## Contributing

Contributions welcome! Feel free to open issues or submit pull requests.
