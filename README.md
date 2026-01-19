# Reaction Reimagined

4-player reaction time game with accessibility features (visual, audio, haptic).

## Architecture

```
ESP32-S3-Touch-LCD-7 (Master)
├── Game logic
├── 7" Display (LVGL 9.x)
├── NeoPixel (60 LEDs, 5 rings)
├── I2S Audio (MAX98357A)
└── ESP-NOW (channel 6)
         │
    ┌────┴────┬────────┬────────┐
    ▼         ▼        ▼        ▼
ESP8266    ESP8266  ESP8266  ESP8266
Joystick 1 Joystick 2 Joystick 3 Joystick 4
```

## Files

| File | Description |
|------|-------------|
| `Protocol.h` | Shared packet format & commands |
| `GameTypes.h` | Game states, constants, types |
| `AudioDefs.h` | Sound file definitions |
| `ESP32S3_Master.ino` | Master controller |
| `ESP8266_Slave.ino` | Joystick controller |

## Quick Start

### 1. Get MAC Addresses

**ESP32-S3:** Upload empty sketch, check Serial output  
**ESP8266:** Same process for each joystick

### 2. Update MACs

**ESP32S3_Master.ino (lines 37-40):**
```cpp
uint8_t mac_stick1[6] = {0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX};
uint8_t mac_stick2[6] = {...};
uint8_t mac_stick3[6] = {...};
uint8_t mac_stick4[6] = {...};
```

**ESP8266_Slave.ino (line 16 & 19):**
```cpp
#define MY_ID  ID_STICK1  // Change per joystick: ID_STICK1-4
uint8_t hostMac[6] = {0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX};  // Master MAC
```

### 3. Upload Audio Files

Upload 29 MP3 files to ESP32-S3 SPIFFS:
```
/1.mp3  /2.mp3  /3.mp3  /4.mp3
/10.mp3  /15.mp3  /20.mp3
/ready.mp3  /player.mp3  /joined.mp3
/fastest.mp3  /join.mp3  /reaction.mp3
/shake.mp3  /over.mp3  /wins.mp3
/beep.mp3  /error.mp3  /victory.mp3
... (see AudioDefs.h for full list)
```

### 4. Flash Firmware

**ESP32-S3:**
- Board: "ESP32-S3 Dev Module"
- Partition: "Huge APP (3MB No OTA)" or custom with SPIFFS
- Upload Speed: 921600

**ESP8266:**
- Board: "NodeMCU 1.0" or "Generic ESP8266"
- Flash Size: "4MB (FS:2MB OTA:~1019KB)"

## Pin Assignments

### ESP32-S3 Master
| Pin | Function |
|-----|----------|
| GPIO8 | NeoPixel DIN |
| GPIO15 | I2S BCLK |
| GPIO16 | I2S LRC |
| GPIO6 | I2S DOUT |

*Verify against available expansion header pins!*

### ESP8266 Joystick
| Pin | Function |
|-----|----------|
| GPIO14 (D5) | Button |
| GPIO12 (D6) | Motor PWM |
| GPIO4 (D2) | I2C SDA (MPU-6050) |
| GPIO5 (D1) | I2C SCL (MPU-6050) |

## Protocol Summary

**Packet (7 bytes):** `[0x0A][DEST][SRC][CMD][DATA_H][DATA_L][CRC8]`

| Command | Value | Direction | Description |
|---------|-------|-----------|-------------|
| CMD_REQ_ID | 0x0D | Stick→Host | Request join |
| CMD_OK | 0x0B | Host→Stick | Join confirmed |
| CMD_COUNTDOWN | 0x25 | Host→Stick | Countdown tick |
| CMD_GAME_START | 0x21 | Host→Stick | Round begins |
| CMD_VIBRATE | 0x23 | Host→Stick | Motor control |
| CMD_REACTION_DONE | 0x26 | Stick→Host | Reaction result |
| CMD_SHAKE_DONE | 0x27 | Stick→Host | Shake result |
| CMD_IDLE | 0x24 | Host→Stick | Return to idle |

## Game Flow

```
IDLE (3s) → JOINING (30s) → COUNTDOWN (4s) →
[REACTION: random wait → GO!] or [SHAKE: count to target] →
RESULTS (5s) → repeat × 5 rounds → FINAL WINNER (10s)
```

## Dependencies

### ESP32-S3
- Adafruit NeoPixel
- ESP8266Audio (by earlephilhower)
- LVGL 9.x
- LovyanGFX

### ESP8266
- Wire (built-in)
- ESP8266WiFi (built-in)

## Flash Size Requirements

| Device | Code | SPIFFS | Total | Available |
|--------|------|--------|-------|-----------|
| ESP32-S3 | ~1.2MB | ~2MB audio | ~3.2MB | 4-16MB ✓ |
| ESP8266 | ~300KB | - | ~300KB | 1MB ✓ |

## Troubleshooting

**No ESP-NOW communication:**
- Verify all MACs are correct
- Ensure channel 6 on all devices
- Check WiFi mode is STA

**Audio not playing:**
- Verify SPIFFS upload succeeded
- Check file paths match AudioDefs.h
- Confirm I2S pins are correct

**MPU-6050 not detected:**
- Check I2C wiring (SDA/SCL not swapped)
- Verify 3.3V power to sensor
- Try I2C scanner sketch

## License

Educational project - University coursework
