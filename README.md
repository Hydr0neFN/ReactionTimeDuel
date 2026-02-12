# Reaction Time Duel

A competitive multiplayer reaction-time game built with ESP32 and ESP8266 microcontrollers, communicating wirelessly over ESP-NOW. Up to 4 players compete across 5 rounds in two game modes using custom wireless joystick controllers.

## Game Modes

### Reaction Mode
NeoPixel rings cycle random colors. When the LEDs freeze to yellow — press your button! The fastest reaction time wins the round. Press too early and you get a penalty.

### Shake Mode
After a 3-2-1 countdown, shake your joystick to reach the target count (10, 15, or 20 shakes). A center ring countdown shows remaining time. First to finish wins.

> Modes are distributed via a **shuffle bag** — both modes are guaranteed to appear before either repeats.

## Hardware

| Component | MCU | Qty | Role |
|-----------|-----|-----|------|
| Host Controller | ESP32 (DevKit-C) | 1 | Game logic, NeoPixel rings, ambient LED strip, I2S audio |
| Joystick | ESP8266 (ESP-12F) | up to 4 | Button input, accelerometer, vibration motor |
| Display | ESP32/ESP8266 | 1 | Game status display (optional, receives ESP-NOW) |

### Host Controller
- **5 NeoPixel rings** (12 LEDs each, 60 total) — player status and game animations
- **89-LED WS2812B ambient strip** — cycles through 6 animations (rainbow, sparkle, meteor, color chase, breathing, fire)
- **I2S MP3 audio** — 25 sound files on SPIFFS for announcements, countdowns, and effects
- **PWM volume control** — hardware gain via amplifier GAIN pin

### Joystick Controller
- **Push button** (GPIO14, active LOW) — reaction input with IRAM interrupt for microsecond precision
- **MPU-6050 accelerometer** — shake detection with high-pass filtered hysteresis (X+Z axes)
- **Vibration motor** — haptic feedback on countdown ticks, GO signal, and completion

## System Architecture

```
                         ┌──────────────────────┐
                         │     ESP32 Host        │
                         │    (Game Logic)       │
                         │                      │
                         │  NeoPixel Rings ×5   │
                         │  WS2812B Strip (89)  │
                         │  I2S Audio + Amp     │
                         │  SPIFFS (25 MP3s)    │
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

## Game Flow

```
IDLE ──> JOIN ──> COUNTDOWN ──> REACTION / SHAKE ──> COLLECT ──> RESULTS ──┐
  ^                                                                        │
  │  (after 5 rounds)    FINAL_WINNER <────────────────────────────────────┘
  └──────────────────────────┘
```

1. **Idle** — Rainbow animation, "press to join" audio prompt
2. **Join** — Players are prompted sequentially (P1 → P2 → P3 → P4). Each slot blinks on its NeoPixel ring for 5 seconds. Any unclaimed joystick can press to claim it. Minimum 2 players required.
3. **Mode Selection** — Shuffle bag picks Reaction or Shake. First-time instructions play once per mode per game.
4. **Countdown** — Shake mode gets a 3-2-1 countdown with synced audio, NeoPixel flash, and haptic vibration. Reaction mode skips countdown and uses a random delay (3s, 5s, or 7s) before GO.
5. **Play** — Reaction: LEDs freeze to yellow = press now. Shake: race to hit target count.
6. **Collect** — Results arrive via ESP-NOW. Yellow warning phase for slow players before disqualification.
7. **Results** — Times displayed for 3s, then winner + scores for 3s.
8. **Final Winner** — After 5 rounds, winner announced with victory fanfare.

## Project Structure

```
ReactionTimeDual/
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
│   │   ├── AudioManager.h          # Non-blocking MP3 queue, I2S output, PWM volume
│   │   └── AudioDefs.h             # Sound ID definitions and SPIFFS file paths
│   └── data/                       # 25 MP3 files (~792 KB) uploaded to SPIFFS
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
        ├── Protocol.h              # Shared protocol (same commands as host)
        ├── GameTypes.h             # Shared constants and types
        ├── AudioDefs.h             # Sound ID definitions
        ├── AudioManager.h          # Audio support definitions
        └── DisplayProtocol.h       # Reference protocol for optional display unit
```

## Building & Flashing

### Prerequisites
- [PlatformIO](https://platformio.org/) IDE or CLI
- ESP32 DevKit-C and ESP-12F boards
- USB cables for programming

### Host Controller (ESP32)

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

## Pin Configuration

### Host (ESP32)

| Pin | Function |
|-----|----------|
| GPIO4 | NeoPixel ring data (5 rings, 60 LEDs) |
| GPIO16 | WS2812B ambient strip data (89 LEDs) |
| GPIO25 | I2S DOUT (audio data) |
| GPIO26 | I2S BCLK (bit clock) |
| GPIO27 | I2S LRC (word select) |
| GPIO33 | Amplifier GAIN (PWM volume control) |

### Joystick (ESP8266)

| Pin | Function |
|-----|----------|
| GPIO14 | Button input (active LOW, internal pullup) |
| GPIO12 | Vibration motor output |
| GPIO4 | I2C SDA (MPU-6050) |
| GPIO5 | I2C SCL (MPU-6050, 400kHz) |

## Communication Protocol

All devices communicate over **ESP-NOW on channel 6** with a 7-byte packet:

| Byte | Field | Description |
|------|-------|-------------|
| 0 | `START` | Always `0x0A` |
| 1 | `DEST_ID` | Destination (0x00=Host, 0x01-04=Sticks, 0x10=Display, 0xFF=Broadcast) |
| 2 | `SRC_ID` | Sender ID |
| 3 | `CMD` | Command byte |
| 4 | `DATA_HIGH` | Data high byte |
| 5 | `DATA_LOW` | Data low byte |
| 6 | `CRC` | CRC8 (polynomial 0x8C) over bytes 0-5 |

### Key Commands

| Command | Hex | Direction | Description |
|---------|-----|-----------|-------------|
| `CMD_REQ_ID` | `0x0D` | Stick → Host | Request to join game |
| `CMD_OK` | `0x0B` | Host → Stick | Join confirmed (data = slot) |
| `CMD_GAME_START` | `0x21` | Host → Stick | Round start (high=mode, low=param) |
| `CMD_GO` | `0x22` | Host → Stick | GO signal — start timing |
| `CMD_COUNTDOWN` | `0x25` | Host → Stick | Countdown tick (3, 2, 1) |
| `CMD_REACTION_DONE` | `0x26` | Stick → Host | Reaction time in ms (`0xFFFF` = penalty) |
| `CMD_SHAKE_DONE` | `0x27` | Stick → Host | Shake time in ms (`0xFFFF` = timeout) |

## Technical Highlights

- **Microsecond Reaction Timing** — Button press captured via `IRAM_ATTR` interrupt on falling edge; time calculated as `micros()` delta from GO signal
- **Non-blocking Architecture** — NeoPixelBus with ESP32 RMT DMA for glitch-free LED output; audio queue with configurable gap between sounds; no `delay()` in game loop
- **High-pass Shake Detection** — EMA low-pass filter (alpha=1/64) removes gravity; hysteresis state machine counts full push-return cycles on X+Z axes
- **Shuffle Bag Mode Selection** — Both Reaction and Shake modes appear before either repeats, preventing streaks
- **Ambient Light Strip** — 89-LED WS2812B strip cycles through 6 procedural animations (rainbow, sparkle, meteor rain, color chase, breathing, fire) on a second RMT channel
- **PWM Volume Control** — Amplifier GAIN pin driven by 25kHz LEDC PWM for smooth analog volume adjustment
- **Accessibility** — Full audio narration (25 MP3 files) covering all game states, player announcements, and instructions

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
