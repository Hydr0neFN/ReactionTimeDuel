# Reaction Time Duel - Firmware

4-player reaction time game in a briefcase with detachable joysticks.

## Hardware Architecture

```
┌─────────────────┐     Shared UART Bus      ┌──────────────────┐
│   ESP32 Host    │◄────────────────────────►│  ESP32-S3 Display│
│  (DEVKIT-V1)    │     (9600 baud)          │    (7" LCD)      │
└────────┬────────┘                          └──────────────────┘
         │
         │ GPIO17 TX ─────────────────┬──────────────────┬───────────────┬────────────────►
         │ GPIO16 RX ◄────────────────┼──────────────────┼───────────────┼─────────────────
         │ GPIO19 GO ─────────────────┼──────────────────┼───────────────┼────────────────►
         │ GPIO18 RST ────────────────┼──────────────────┼───────────────┼────────────────►
         │                            ▼                  ▼               ▼
         │                       ┌────────┐        ┌────────┐      ┌────────┐      ┌────────┐
         │                       │Stick 1 │        │Stick 2 │      │Stick 3 │      │Stick 4 │
         │                       │ATtiny85│        │ATtiny85│      │ATtiny85│      │ATtiny85│
         │                       └────────┘        └────────┘      └────────┘      └────────┘
```

## Pin Assignments (from schematic - PCB printed, DO NOT CHANGE)

### ESP32 Host (DEVKIT-V1)

| GPIO | Function | Connection |
|------|----------|------------|
| 17 | TX2 | → D+ → All joysticks RX (PB0) + Display RX |
| 16 | RX2 | ← D- ← All joysticks TX (PB1) + Display TX (via voltage divider) |
| 18 | CC1 | → RST signal → All joysticks PB5 (hardware reset) |
| 19 | CC2 | → GO signal → All joysticks PB2 (hardware timing sync) |
| 4 | DIN | → NeoPixel rings (60 LEDs = 5 × 12) |
| 23 | DINS | → I2S DOUT → MAX98357A amplifier |
| 26 | BCLK | → I2S BCLK |
| 25 | LRC | → I2S LRC |

### ATtiny85-20P Joystick

| Pin | PB# | Function | Notes |
|-----|-----|----------|-------|
| 5 | PB0 | UART RX + I2C SDA | **SHARED!** |
| 6 | PB1 | UART TX | |
| 7 | PB2 | I2C SCL + GO input | **SHARED!** |
| 2 | PB3 | Button | INPUT_PULLUP, R5/C4 debounce |
| 3 | PB4 | Motor PWM | via R6 → Q2 BC547 |
| 1 | PB5 | RESET | Connected to CC1/RST |

### USB Type-C Connector Mapping

| USB Pin | Signal | Direction |
|---------|--------|-----------|
| D+ | RX (from host TX) | Host → Joystick |
| D- | TX (to host RX) | Joystick → Host |
| CC1 | RST | Host → Joystick |
| CC2 | GO | Host → Joystick |
| VBUS | +5V | Power |

## Protocol

### Packet Format (7 bytes)
```
[0x0A][DEST_ID][SRC_ID][CMD][DATA_HIGH][DATA_LOW][CRC8]
```

### Collision Prevention
- **Token-based transmission**: Joysticks only transmit when granted permission
- **Hardware GO signal**: GPIO19 pulse for precise timing sync (avoids UART latency)
- **Display exception**: May transmit TOUCH_SKIP without token (rare, low collision risk)

## Shared Pin Strategy (ATtiny85)

PB0 and PB2 are shared between UART and I2C:

| Phase | PB0 Usage | PB2 Usage |
|-------|-----------|-----------|
| ID Assignment | UART RX | GO input (unused) |
| Idle | UART RX | GO input |
| Waiting GO | UART RX | GO input (waiting for HIGH) |
| Button Mode | UART RX | GO input (already triggered) |
| Shake Mode | **I2C SDA** | **I2C SCL** (UART degraded) |
| Finished | UART RX | GO input |

**Key insight**: I2C (MPU-6050) is only needed during SHAKE_MODE. During this phase, UART RX may be degraded but commands are not expected.

## Known Hardware Issue

**SD Card Pin Conflict RESOLVED:** Original soundProgram used VSPI (GPIO18/19) which conflicts with CC1/CC2 signals. Solution: Use HSPI instead.

| Original (VSPI) | Corrected (HSPI) | Notes |
|-----------------|------------------|-------|
| GPIO18 (SCK) | GPIO14 | CC1/RST uses GPIO18 |
| GPIO19 (MISO) | GPIO12 | CC2/GO uses GPIO19 |
| GPIO23 (MOSI) | GPIO13 | I2S DOUT uses GPIO23 |
| GPIO5 (CS) | GPIO5 | No conflict |

## Audio System

### Pin Configuration (verify against your schematic!)

| Function | GPIO | Original soundProgram | Notes |
|----------|------|----------------------|-------|
| I2S DOUT | 25 or 23 | 25 | **VERIFY** which your PCB uses |
| I2S BCLK | 26 | 26 | OK |
| I2S LRC | 22 or 25 | 22 | **VERIFY** which your PCB uses |
| SD SCK | 14 | ~~18~~ | Changed to HSPI |
| SD MISO | 12 | ~~19~~ | Changed to HSPI |
| SD MOSI | 13 | ~~23~~ | Changed to HSPI |
| SD CS | 5 | 5 | OK |

### SD Card File Structure
```
/1.mp3   - "One"
/2.mp3   - "Two"
/3.mp3   - "Three"
/4.mp3   - "Four"
/5.mp3   - "Ten"
/6.mp3   - "Fifteen"
/7.mp3   - "Twenty"
/8.mp3   - "Get Ready"
/9.mp3   - "3-2-1-Go"
/10.mp3  - "Player"
...
/28.mp3  - "Button Click"
```

### Usage in Code
```cpp
#include "AudioManager.h"

AudioManager audio;

void setup() {
  // Initialize with 50% volume (0.0 to 4.0)
  if (!audio.begin(0.5)) {
    Serial.println("Audio init failed!");
  }
}

void loop() {
  audio.update();  // CRITICAL: Must call every loop iteration!
  
  // Queue single sounds
  audio.queueSound(SND_GET_READY);
  audio.queueSound(SND_BEEP);
  
  // Convenience methods
  audio.playPlayerNumber(1);    // "Player" + "1"
  audio.playPlayerWins(2);      // "Player" + "2" + "Wins"
  audio.playCountdown(3);       // "3" (or tick sound)
  
  // Control
  audio.setVolume(0.8);         // Change volume
  audio.stop();                 // Stop and clear queue
  audio.isPlaying();            // Check if playing
  audio.getQueueCount();        // Items in queue
}
```

### Non-Blocking Design
- `audio.update()` must be called every loop iteration
- Sounds queue up and play sequentially
- Queue holds up to 16 sounds
- Thread-safe for dual-core operation

## Game Flow

1. **IDLE** → Rainbow animation, wait for display touch
2. **ASSIGN_IDS** → Prompt "Press Player 1-4", button assigns ID
3. **COUNTDOWN** → 3-2-1 with red blinks + vibration
4. **REACTION/SHAKE** → Game runs until completion/timeout
5. **COLLECT_RESULTS** → Token-based result collection
6. **SHOW_RESULTS** → Display times, update scores
7. **Repeat** for 5 rounds
8. **FINAL_WINNER** → Announce winner, return to IDLE

## Building

### ATtiny85
- **Board**: ATTinyCore (ATtiny85, 8MHz internal)
- **Programmer**: USBasp or Arduino as ISP
- **Libraries**: None (custom software I2C/UART to save flash)

### ESP32 Host
- **Board**: ESP32 Dev Module (or DOIT ESP32 DEVKIT V1)
- **Libraries**:
  - Adafruit NeoPixel
  - **ESP8266Audio** ← Yes, this name is correct! Despite the name, it fully supports ESP32. Install via Library Manager.

### Library Installation
1. Arduino IDE → Sketch → Include Library → Manage Libraries
2. Search "ESP8266Audio" → Install (by Earle F. Philhower)
3. Search "Adafruit NeoPixel" → Install

> **Note:** ESP8266Audio was originally written for ESP8266 but fully supports ESP32 (and works better on ESP32 due to more RAM). The author never renamed it.

## Testing Checklist

- [ ] SD card mounts (check serial output)
- [ ] MP3 files play (use soundProgram_Fixed.ino first)
- [ ] UART loopback (TX→RX)
- [ ] ID assignment sequence
- [ ] Hardware GO signal timing
- [ ] NeoPixel animations
- [ ] Button debounce
- [ ] Motor PWM
- [ ] MPU-6050 shake detection
- [ ] Token-based transmission
- [ ] 5-round game completion
- [ ] Audio plays during game states

## Files

| File | Description |
|------|-------------|
| Protocol.h | Shared definitions (IDs, commands, pins, CRC8, sound catalog) |
| AudioManager.h | Non-blocking audio queue system with ID-based playback |
| ATtiny85_Joystick.ino | Joystick controller firmware |
| ESP32_Host.ino | Main game controller firmware (with audio integration) |
| soundProgram_Fixed.ino | Simple audio test sketch (for debugging) |

## Dependencies

| Library | Source | Used By |
|---------|--------|---------|
| ESP8266Audio | Library Manager | ESP32_Host (AudioManager) |
| Adafruit NeoPixel | Library Manager | ESP32_Host |
| ATTinyCore | Board Manager | ATtiny85_Joystick |
