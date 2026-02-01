# Reaction Time Duel

A competitive multiplayer game system that tests players' reaction times and physical coordination through wireless joystick controllers.

## Overview

Reaction Time Duel is a distributed game system built on ESP32/ESP8266 microcontrollers communicating via ESP-NOW protocol. Up to 4 players compete across 5 rounds in two exciting game modes.

## Game Modes

### Reaction Mode
Press the button as fast as possible when the signal appears! The player with the quickest reaction time wins the round.

### Shake Mode
Shake your joystick to reach the target count (10, 15, or 20 shakes) as fast as possible! First to complete wins.

## Hardware Components

| Component | Microcontroller | Quantity | Purpose |
|-----------|-----------------|----------|---------|
| Host Controller | ESP32 | 1 | Central game logic, audio, LED rings |
| Joystick | ESP8266 | 4 | Player input devices with accelerometer |
| Display | ESP8266/ESP32 | 1 | Game status display (optional) |

### Host Controller Features
- 5 NeoPixel LED rings (12 LEDs each, 60 total)
- I2S audio output with MP3 playback
- ESP-NOW wireless communication

### Joystick Features
- Push button for reaction input
- MPU-6050 accelerometer for shake detection
- Vibration motor for haptic feedback

## System Architecture

```
                    ┌─────────────────┐
                    │   ESP32 Host    │
                    │  (Game Logic)   │
                    │                 │
                    │  - NeoPixel ×5  │
                    │  - Audio I2S    │
                    └────────┬────────┘
                             │ ESP-NOW
          ┌──────────────────┼──────────────────┐
          │                  │                  │
    ┌─────┴─────┐      ┌─────┴─────┐      ┌─────┴─────┐
    │ Joystick 1│      │ Joystick 2│      │ Joystick 3│ ... ×4
    │  ESP8266  │      │  ESP8266  │      │  ESP8266  │
    │           │      │           │      │           │
    │ - Button  │      │ - Button  │      │ - Button  │
    │ - MPU6050 │      │ - MPU6050 │      │ - MPU6050 │
    │ - Vibrate │      │ - Vibrate │      │ - Vibrate │
    └───────────┘      └───────────┘      └───────────┘
```

## Game Flow

1. **Join Phase** - Players press their joystick buttons to join (2-4 players)
2. **Mode Selection** - System randomly selects Reaction or Shake mode
3. **Countdown** - 3-2-1 countdown with audio and haptic feedback
4. **Play** - Players compete based on the selected mode
5. **Results** - Round winner announced, scores updated
6. **Repeat** - 5 rounds total, then final winner declared

## Project Structure

```
ReactionTimeDual/
├── README.md
├── ReactionTimerHost/          # ESP32 Host Controller
│   ├── src/main.cpp
│   ├── include/
│   │   ├── Protocol.h
│   │   ├── GameTypes.h
│   │   ├── AudioManager.h
│   │   └── DisplayProtocol.h
│   ├── data/                   # MP3 audio files
│   └── platformio.ini
│
└── ReactionTimerSlave/         # ESP8266 Joysticks (×4)
    ├── src/main.cpp
    ├── include/
    │   ├── Protocol.h
    │   └── GameTypes.h
    └── platformio.ini
```

## Building & Flashing

### Prerequisites
- [PlatformIO](https://platformio.org/) IDE or CLI
- ESP32 and ESP8266 development boards
- USB cables for programming

### Host Controller (ESP32)

```bash
cd ReactionTimerHost
pio run -t upload
pio run -t uploadfs    # Upload audio files to SPIFFS
```

### Joysticks (ESP8266)

Each joystick has a unique build target:

```bash
cd ReactionTimerSlave
pio run -e stick1 -t upload    # Joystick 1
pio run -e stick2 -t upload    # Joystick 2
pio run -e stick3 -t upload    # Joystick 3
pio run -e stick4 -t upload    # Joystick 4
```

## Pin Configuration

### Host (ESP32)
| Pin | Function |
|-----|----------|
| GPIO4 | NeoPixel Data |
| GPIO18 | Reset Line (Joysticks) |
| GPIO25 | I2S DOUT |
| GPIO26 | I2S BCLK |
| GPIO27 | I2S LRC |

### Joystick (ESP8266)
| Pin | Function |
|-----|----------|
| GPIO14 | Button Input |
| GPIO12 | Vibration Motor |
| GPIO4 | I2C SDA (MPU-6050) |
| GPIO5 | I2C SCL (MPU-6050) |

## Communication Protocol

All devices communicate using ESP-NOW with a custom packet format:

| Byte | Content |
|------|---------|
| 0 | Start (0x0A) |
| 1 | Destination ID |
| 2 | Source ID |
| 3 | Command |
| 4 | Data High |
| 5 | Data Low |
| 6 | CRC8 |

## Technical Highlights

- **Precision Timing**: Interrupt-based microsecond-accurate reaction measurement
- **Non-blocking Audio**: Queue-based MP3 playback during gameplay
- **Haptic Feedback**: Vibration motor pulses synchronized with game events
- **Shuffle Bag Algorithm**: Ensures balanced mode distribution across rounds
- **Accessibility**: Complete audio narration for visually impaired players

## Dependencies

### Host (ESP32)
- Adafruit NeoPixel v1.12.0
- ESP8266Audio v1.9.7

### Joystick (ESP8266)
- ESP8266WiFi (built-in)
- Wire (I2C for MPU-6050)

## License

MIT License

## Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues for bugs and feature requests.