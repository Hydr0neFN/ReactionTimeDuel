# Reaction Time Duel - Firmware Documentation

**"Made for most, fun for all"**

## ♿ Accessibility

This game implements multi-sensory feedback for inclusive play:

### 🦻 For Hearing Impaired Players
- **NeoPixel LED rings**: Red/green status, countdown blinks, GO signal (LEDs stop on fixed color)
- **7" Display**: All game state, countdown numbers, reaction times, winner announcements
- **Color coding**: Green=success, Red=penalty, Rainbow=idle/celebration

### 👁️ For Visually Impaired Players
- **Vibration motor**: Countdown 200ms pulses, GO 500ms strong, button confirm 100ms, penalty double-buzz
- **Audio announcements**: Voice callouts for all events
- **Tactile button**: Large, easy-to-find, hardware debounced

### 🎯 Multi-Sensory Event Mapping

| Event | Visual (LEDs) | Visual (Display) | Audio | Haptic |
|:------|:--------------|:-----------------|:------|:-------|
| Player joins | Green ring | "Player X ready" | Voice | 100ms buzz |
| Countdown | Blink red | "3, 2, 1" | Beep+voice | 200ms pulse |
| GO signal | Fixed green | "GO!" | Beep | 500ms strong |
| Button press | — | — | — | 100ms confirm |
| Win | Rainbow | "WINNER" | Fanfare | — |
| Penalty/Timeout | Blink red 3× | Red, no time | Error tone | Double-buzz |

---

## Hardware Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        BRIEFCASE                                 │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐       │
│  │ ESP32-DEVKIT │    │  ESP32-S3    │    │  MAX98357A   │       │
│  │   (Host)     │───▶│  7" Display  │    │   (Audio)    │       │
│  │              │    │              │    │              │       │
│  └──────┬───────┘    └──────────────┘    └──────────────┘       │
│         │ UART+GO+RST                                            │
│         │                                                        │
│    ┌────┴────┬─────────┬─────────┐                              │
│    ▼         ▼         ▼         ▼                              │
│ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐    ┌──────────────┐        │
│ │Stick1│ │Stick2│ │Stick3│ │Stick4│    │ NeoPixel ×5  │        │
│ └──────┘ └──────┘ └──────┘ └──────┘    └──────────────┘        │
│                                         (Display: UART only)    │
└─────────────────────────────────────────────────────────────────┘
```

---

## Pin Assignments

### ESP32-DEVKIT-V1 (Host)

| GPIO | Function | Connection |
|:-----|:---------|:-----------|
| 17 | UART TX | D+ → Joystick RX (PB0) |
| 16 | UART RX | D- ← Joystick TX (PB1) via voltage divider |
| 18 | RST out | CC1 → Joystick PB5 (Reset) |
| 19 | GO out | CC2 → Joystick PB3 (GO signal) |
| 4 | NeoPixel | DIN to LED rings |
| 23 | I2S DOUT | MAX98357A DIN |
| 26 | I2S BCLK | MAX98357A BCLK |
| 25 | I2S LRC | MAX98357A LRC |

### ATtiny85 (Joystick) - CORRECTED

| PB# | Arduino Pin | Function | Notes |
|:----|:------------|:---------|:------|
| PB0 | 0 | RX + SDA | UART receive, I2C data (shared) |
| PB1 | 1 | TX + Motor | UART transmit, Motor via Q2 transistor (shared) |
| PB2 | 2 | SCL | I2C clock for MPU-6050 |
| PB3 | 3 | GO | Hardware GO signal from ESP32 GPIO19 |
| PB4 | 4 | Button | Switch input (INPUT_PULLUP) |
| PB5 | 5 | RESET | Reset signal from ESP32 GPIO18 |

### Type-C Cable Mapping

| USB-C Pin | Signal | Direction |
|:----------|:-------|:----------|
| VBUS | +5V | Power |
| GND | GND | Power |
| D+ | RX (to ATtiny PB0) | Host → Stick |
| D- | TX (from ATtiny PB1) | Stick → Host |
| CC1 | RST | Host → Stick |
| CC2 | GO | Host → Stick |

---

## Communication Protocol

### Packet Format (7 bytes)

| Byte | Field | Description |
|:-----|:------|:------------|
| 0 | START | Always 0x0A |
| 1 | DEST | Destination ID |
| 2 | SRC | Source ID |
| 3 | CMD | Command byte |
| 4 | DATA_H | Data high byte |
| 5 | DATA_L | Data low byte |
| 6 | CRC | CRC8 checksum |

### Device IDs

| ID | Device |
|:---|:-------|
| 0x00 | Host (ESP32) |
| 0x01-0x04 | Joysticks 1-4 |
| 0x10 | Display |
| 0xFF | Broadcast |

### Commands

| CMD | Name | Direction | DATA_H | DATA_L |
|:----|:-----|:----------|:-------|:-------|
| 0x10 | PING | Host→Stick | — | — |
| 0x11 | PONG | Stick→Host | — | — |
| 0x12 | GET_BUTTON | Both | — | 0/1 |
| 0x13 | GET_ACCEL | Both | mag_H | mag_L |
| 0x20 | ASSIGN_ID | Host→Stick | — | new_id |
| 0x21 | JOIN_ACK | Stick→Host | — | id |
| 0x22 | COUNTDOWN | Host→Stick | — | count |
| 0x23 | VIBRATE | Host→Stick | — | dur×10ms or 0xFF=GO |
| 0x24 | RESULT | Stick→Host | time_H | time_L |
| 0x25 | SHAKE_COUNT | Stick→Host | count_H | count_L |
| 0x30 | IDLE | Host→All | — | — |

---

## Game State Machine

```
        ┌─────────────────────────────────────────┐
        │                                         │
        ▼                                         │
   ┌─────────┐    <2 players     ┌──────┐        │
   │ ASSIGN  │───────────────────│ IDLE │        │
┌─▶│  _IDS   │    after 15s      │      │────────┘
│  └────┬────┘                   └──────┘  touch/60s
│       │ 2+ players                          
│       ▼                                     
│  ┌─────────┐                                
│  │COUNTDOWN│ 5 seconds total                
│  └────┬────┘                                
│       │                                     
│       ▼                                     
│  ┌─────────┐    ┌─────────┐                 
│  │REACTION │ or │ SHAKE   │                 
│  │  MODE   │    │  MODE   │                 
│  └────┬────┘    └────┬────┘                 
│       │              │                      
│       ▼              ▼                      
│  ┌─────────────────────┐                    
│  │   SHOW_RESULTS      │                    
│  └──────────┬──────────┘                    
│             │                               
│     ┌───────┴───────┐                       
│     │ more rounds?  │                       
│     └───────┬───────┘                       
│        yes  │  no                           
│             ▼                               
│       ┌───────────┐                         
│       │FINAL_WINNER│                        
│       └─────┬─────┘                         
│             │                               
└─────────────┘                               
```

---

## Files

| File | Description |
|:-----|:------------|
| `Protocol.h` | Shared protocol definitions |
| `ESP32_Host.ino` | Main game logic (host) |
| `ESP32_HardwareTest.ino` | Host hardware test |
| `ATtiny85_Joystick.ino` | Main joystick firmware |
| `ATtiny85_HardwareTest.ino` | Joystick test (needs host) |
| `ATtiny85_BasicTest.ino` | Standalone joystick test (no host needed) |
| `AudioManager.h` | Audio queue system |
| `DisplayProtocol.h` | Display commands |

---

## Testing

### Standalone Joystick Test (ATtiny85_BasicTest.ino)

**No ESP32 needed.** Works at 1 MHz clock.

| Event | Expected |
|:------|:---------|
| Power on | 3 short buzzes → silence |
| Press button | 1 short buzz |

### Host + Joystick Test

1. Upload `ESP32_HardwareTest.ino` to ESP32
2. Upload `ATtiny85_HardwareTest.ino` to ATtiny85 (requires 8 MHz clock)
3. Connect joystick to host
4. Open Serial Monitor (115200 baud)
5. Send commands:

| Key | Test |
|:---:|:-----|
| `6` | UART ping (should see PONG) |
| `7` | Read button state |
| `4` | Pulse GO signal (joystick vibrates) |
| `v` | Vibrate via UART command |
| `0` | Full test sequence |

---

## Clock Speed Requirements

| Code | Minimum Clock |
|:-----|:--------------|
| ATtiny85_BasicTest.ino | 1 MHz ✓ |
| ATtiny85_HardwareTest.ino | 8 MHz |
| ATtiny85_Joystick.ino | 8 MHz |

**To set 8 MHz:**
1. Tools → Clock Source → "8 MHz (internal)"
2. Tools → Burn Bootloader
3. Upload code

---

## Troubleshooting

| Problem | Cause | Solution |
|:--------|:------|:---------|
| Motor always on | Floating GO pin | Use BasicTest or connect to ESP32 |
| No UART response | Wrong clock speed | Set 8 MHz, burn bootloader |
| Motor never spins | Wrong pin (was PB4) | Now fixed to PB1 |
| Button no response | Wrong pin (was PB3) | Now fixed to PB4 |
