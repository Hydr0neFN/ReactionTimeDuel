# AI Agent Briefing: Display Integration for Reaction Game

## Your Role
You are helping integrate LVGL 9.x display code with a reaction time game on ESP32-S3. The game logic is complete - you need to connect UI updates to game state changes.

---

## System Overview

**Hardware**: Waveshare ESP32-S3-Touch-LCD-7 (800×480)
**Framework**: LVGL 9.x + LovyanGFX
**Architecture**: Dual-core (Core 0 = game logic, Core 1 = LVGL rendering)

```
┌─────────────────────────────────────────┐
│           ESP32-S3 (Single Device)      │
│                                         │
│  Core 0                Core 1           │
│  ┌─────────────┐      ┌─────────────┐  │
│  │ Game Logic  │      │ LVGL Task   │  │
│  │ ESP-NOW     │      │ NeoPixel    │  │
│  │ Audio       │      │ Touch Input │  │
│  └──────┬──────┘      └──────┬──────┘  │
│         │    Mutex Protected │         │
│         └────────────────────┘         │
└─────────────────────────────────────────┘
```

---

## Game States & Required UI

The game has 8 states. Your UI must respond to each:

| State | What to Display | Triggered By |
|-------|-----------------|--------------|
| `GAME_IDLE` | Attract screen, logo, rainbow animation | Game start/end |
| `GAME_JOINING` | "Press to Join", player slots (P1-P4) | After idle timeout |
| `GAME_COUNTDOWN` | Mode announcement + "3", "2", "1" | After 2+ players join |
| `GAME_REACTION_WAIT` | "Watch the LEDs..." or "Get Ready" | After countdown |
| `GAME_REACTION_ACTIVE` | "GO!" or "PRESS NOW!" | Random delay elapsed |
| `GAME_SHAKE_ACTIVE` | "SHAKE!" + target count | After countdown |
| `GAME_RESULTS` | Player times, round winner, scores | Round complete |
| `GAME_FINAL` | Final winner, trophy, "Game Over" | After 5 rounds |

---

## Thread Safety (CRITICAL!)

LVGL is NOT thread-safe. All UI updates from game logic MUST use this pattern:

```cpp
// Mutex is defined globally
extern SemaphoreHandle_t lvglMutex;

// Safe UI update wrapper
void ui_update(void (*updateFn)()) {
  if (xSemaphoreTake(lvglMutex, pdMS_TO_TICKS(10))) {
    updateFn();
    xSemaphoreGive(lvglMutex);
  }
}

// Usage from game logic:
void ui_showGo_impl() {
  lv_obj_add_flag(ui_imgStart, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(ui_imgGo, LV_OBJ_FLAG_HIDDEN);
}

// Called from game state machine:
ui_update(ui_showGo_impl);
```

---

## UI Functions to Implement

These functions are called by the game logic. Implement them based on your SquareLine Studio exports:

```cpp
// Required UI update functions (implement these)
void ui_showIdle_impl();           // Attract/idle screen
void ui_showJoining_impl();        // Join prompt with player slots
void ui_showCountdown_impl(uint8_t num);  // Show 3, 2, 1
void ui_showMode_impl(uint8_t mode, uint8_t param);  // "Reaction Mode" or "Shake 15"
void ui_showGo_impl();             // "GO!" screen
void ui_showWaiting_impl();        // "Watch LEDs..." 
void ui_showResults_impl();        // Times and winner
void ui_showFinal_impl(uint8_t winner);  // Final winner celebration

// Player status updates
void ui_setPlayerJoined_impl(uint8_t player, bool joined);  // P1-P4 join indicators
void ui_setPlayerTime_impl(uint8_t player, uint16_t timeMs);  // Show reaction time
void ui_setPlayerScore_impl(uint8_t player, uint8_t score);   // Update score
void ui_highlightWinner_impl(uint8_t player);  // Highlight round winner
```

---

## SquareLine Studio Objects (Expected)

Based on the existing `ui_MainScreen.c`, these LVGL objects exist:

```cpp
// Declared in ui_MainScreen.h
extern lv_obj_t *ui_MainScreen;      // Main screen container
extern lv_obj_t *ui_centerCircle;    // Center decoration
extern lv_obj_t *ui_imgStart;        // "START" image
extern lv_obj_t *ui_imgGo;           // "GO!" image
extern lv_obj_t *ui_player1;         // Player 1 indicator
extern lv_obj_t *ui_player2;         // Player 2 indicator
extern lv_obj_t *ui_player3;         // Player 3 indicator
extern lv_obj_t *ui_player4;         // Player 4 indicator
extern lv_obj_t *ui_player1Ready;    // Player 1 ready indicator (green)
extern lv_obj_t *ui_player2Ready;    // ...
extern lv_obj_t *ui_player3Ready;
extern lv_obj_t *ui_player4Ready;
```

---

## LVGL 9.x API Notes

Key differences from LVGL 8.x:

```cpp
// Showing/hiding objects
lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);    // Hide
lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);  // Show

// Labels
lv_label_set_text(label, "GO!");
lv_label_set_text_fmt(label, "%d", value);

// Styles (LVGL 9.x)
lv_obj_set_style_bg_color(obj, lv_color_hex(0x00FF00), LV_PART_MAIN);
lv_obj_set_style_text_color(obj, lv_color_hex(0xFF0000), LV_PART_MAIN);

// Animations
lv_anim_t a;
lv_anim_init(&a);
lv_anim_set_var(&a, obj);
lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
lv_anim_set_duration(&a, 300);
lv_anim_start(&a);
```

---

## Data Flow Example

When a player joins:

```
1. ESP8266 joystick sends CMD_REQ_ID via ESP-NOW
2. Game logic (Core 0) receives packet, assigns player slot
3. Game logic calls: ui_update([]{ ui_setPlayerJoined_impl(slot, true); });
4. LVGL task (Core 1) acquires mutex, updates UI, releases mutex
5. Player slot shows green indicator
```

When showing results:

```cpp
// In game logic GAME_RESULTS state entry:
for (int i = 0; i < 4; i++) {
  if (players[i].joined) {
    uint16_t time = players[i].reactionTime;
    uint8_t score = players[i].score;
    
    // Must use lambda or function pointer for thread safety
    ui_update([&]{ 
      ui_setPlayerTime_impl(i, time);
      ui_setPlayerScore_impl(i, score);
    });
  }
}

uint8_t winner = findWinner();
if (winner != 0xFF) {
  ui_update([&]{ ui_highlightWinner_impl(winner); });
}
```

---

## Game Constants

```cpp
#define MAX_PLAYERS       4
#define TOTAL_ROUNDS      5

// Time display
// 0xFFFF = timeout or penalty (show "---" or red indicator)
// Otherwise show in milliseconds: "234 ms"

// Shake targets
const uint8_t SHAKE_TARGETS[] = {10, 15, 20};

// Game modes
#define MODE_REACTION     0x01
#define MODE_SHAKE        0x02
```

---

## Touch Input (Optional)

If you want touch-to-skip functionality:

```cpp
// In LVGL event callback
void touch_event_cb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
    // Set a flag that game logic can check
    extern volatile bool touchSkipRequested;
    touchSkipRequested = true;
  }
}

// Attach to screen
lv_obj_add_event_cb(ui_MainScreen, touch_event_cb, LV_EVENT_CLICKED, NULL);
```

---

## File Structure

```
project/
├── ESP32S3_Master.ino    # Main firmware (game logic + audio + neopixel)
├── display.cpp           # LovyanGFX initialization (YOUR CODE)
├── display.h             # Display header (YOUR CODE)
├── lgfx_conf.cpp         # LovyanGFX config (YOUR CODE)
├── lgfx_conf.h           # LovyanGFX header (YOUR CODE)
├── ui.c                  # SquareLine export (YOUR CODE)
├── ui.h                  # SquareLine export (YOUR CODE)
├── ui_MainScreen.c       # SquareLine screen (YOUR CODE)
├── ui_MainScreen.h       # SquareLine screen header (YOUR CODE)
├── ui_helpers.h          # SquareLine helpers (YOUR CODE)
├── ui_events.h           # SquareLine events (create if missing)
└── lv_conf.h             # LVGL configuration (YOUR CODE)
```

---

## Quick Checklist

- [ ] Create `ui_events.h` if SquareLine didn't export it (can be empty)
- [ ] Implement `ui_*_impl()` functions using your LVGL objects
- [ ] Wrap ALL UI updates with `ui_update()` for thread safety
- [ ] Test each game state transition shows correct screen
- [ ] Handle 0xFFFF time values (timeout/penalty display)
- [ ] Add player score display elements if not present
- [ ] Consider adding countdown number label (large, centered)

---

## Questions to Ask Yourself

1. What LVGL objects did SquareLine Studio generate?
2. Are there labels for countdown numbers and reaction times?
3. How should penalties (0xFFFF) be displayed?
4. Do you need additional screens or just show/hide elements?
5. Should touch input trigger game actions?

---

## Example Implementation Skeleton

```cpp
// ui_game_integration.cpp - Add this file

#include "ui.h"
#include "lvgl.h"

// Labels you may need to add in SquareLine
extern lv_obj_t *ui_lblCountdown;  // Large "3", "2", "1"
extern lv_obj_t *ui_lblMode;       // "Reaction Mode" / "Shake 15"
extern lv_obj_t *ui_lblP1Time;     // "234 ms"
extern lv_obj_t *ui_lblP2Time;
extern lv_obj_t *ui_lblP3Time;
extern lv_obj_t *ui_lblP4Time;

void ui_showIdle_impl() {
  lv_obj_clear_flag(ui_centerCircle, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui_imgGo, LV_OBJ_FLAG_HIDDEN);
  // Add attract animation here
}

void ui_showCountdown_impl(uint8_t num) {
  if (ui_lblCountdown) {
    lv_label_set_text_fmt(ui_lblCountdown, "%d", num);
    lv_obj_clear_flag(ui_lblCountdown, LV_OBJ_FLAG_HIDDEN);
  }
}

void ui_showGo_impl() {
  if (ui_lblCountdown) lv_obj_add_flag(ui_lblCountdown, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(ui_imgGo, LV_OBJ_FLAG_HIDDEN);
}

void ui_setPlayerJoined_impl(uint8_t player, bool joined) {
  lv_obj_t *readyIndicators[] = {ui_player1Ready, ui_player2Ready, 
                                  ui_player3Ready, ui_player4Ready};
  if (player < 4 && readyIndicators[player]) {
    if (joined) {
      lv_obj_clear_flag(readyIndicators[player], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(readyIndicators[player], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void ui_setPlayerTime_impl(uint8_t player, uint16_t timeMs) {
  lv_obj_t *timeLabels[] = {ui_lblP1Time, ui_lblP2Time, 
                            ui_lblP3Time, ui_lblP4Time};
  if (player < 4 && timeLabels[player]) {
    if (timeMs == 0xFFFF) {
      lv_label_set_text(timeLabels[player], "---");
      lv_obj_set_style_text_color(timeLabels[player], 
                                   lv_color_hex(0xFF0000), LV_PART_MAIN);
    } else {
      lv_label_set_text_fmt(timeLabels[player], "%d ms", timeMs);
      lv_obj_set_style_text_color(timeLabels[player], 
                                   lv_color_hex(0x00FF00), LV_PART_MAIN);
    }
  }
}
```

---

*Document for AI agent integration assistance - Reaction Reimagined v2.0*

---

## APPENDIX A: Protocol Commands

All ESP-NOW packets are 7 bytes: `[0x0A][DEST][SRC][CMD][DATA_H][DATA_L][CRC8]`

### Device IDs
```cpp
#define ID_HOST           0x00  // ESP32-S3 (this device)
#define ID_STICK1         0x01  // Joystick 1
#define ID_STICK2         0x02  // Joystick 2
#define ID_STICK3         0x03  // Joystick 3
#define ID_STICK4         0x04  // Joystick 4
```

### Commands: Host → Joysticks
| Command | Value | Data | Description |
|---------|-------|------|-------------|
| `CMD_OK` | 0x0B | assigned_id | Acknowledge join |
| `CMD_GAME_START` | 0x21 | mode<<8 \| param | Start round (mode: 0x01=reaction, 0x02=shake; param: shake target) |
| `CMD_VIBRATE` | 0x23 | 0xFF=GO, else ms/10 | Vibration command |
| `CMD_IDLE` | 0x24 | 0 | Return to idle |
| `CMD_COUNTDOWN` | 0x25 | 3, 2, or 1 | Countdown tick |

### Commands: Joysticks → Host
| Command | Value | Data | Description |
|---------|-------|------|-------------|
| `CMD_REQ_ID` | 0x0D | 0 | Request to join game |
| `CMD_REACTION_DONE` | 0x26 | time_ms (0xFFFF=penalty) | Reaction result |
| `CMD_SHAKE_DONE` | 0x27 | time_ms (0xFFFF=timeout) | Shake result |

---

## APPENDIX B: Audio Sound IDs

Audio files stored in SPIFFS. Queue sounds with `queueSound(id)`.

### Number Sounds
| ID | File | Plays When |
|----|------|------------|
| 1 | /1.mp3 | "one" - Player 1 announcement |
| 2 | /2.mp3 | "two" - Player 2 / countdown |
| 3 | /3.mp3 | "three" - Player 3 / countdown |
| 4 | /4.mp3 | "four" - Player 4 |
| 5 | /10.mp3 | "ten" - Shake target |
| 6 | /15.mp3 | "fifteen" - Shake target |
| 7 | /20.mp3 | "twenty" - Shake target |

### Voice Phrases
| ID | File | Plays When |
|----|------|------------|
| 8 | /ready.mp3 | "get ready" - Before countdown |
| 10 | /player.mp3 | "player" - Before number |
| 11 | /joined.mp3 | "ready" - After player joins |
| 14 | /fastest.mp3 | "fastest" - Round winner |
| 15 | /join.mp3 | "press to join" - Join phase |
| 17 | /reaction.mp3 | "reaction mode" - Mode announce |
| 19 | /shake.mp3 | "shake it" - Mode announce |
| 22 | /over.mp3 | "game over" - Final screen |
| 23 | /wins.mp3 | "wins" - After player number |
| 27 | /victory.mp3 | Victory fanfare - Final winner |

### Sound Effects
| ID | File | Plays When |
|----|------|------------|
| 24 | /beep.mp3 | GO signal |
| 25 | /error.mp3 | Penalty/timeout |
| 28 | /click.mp3 | Button feedback |

### Audio Sequences (Examples)
```cpp
// Player 2 joins:
queueSound(10);  // "player"
queueSound(2);   // "two"
queueSound(11);  // "ready"

// Countdown:
queueSound(3);   // "three"
// wait 1 sec
queueSound(2);   // "two"
// wait 1 sec
queueSound(1);   // "one"

// Round winner (Player 3):
queueSound(10);  // "player"
queueSound(3);   // "three"
queueSound(14);  // "fastest"

// Final winner (Player 1):
queueSound(27);  // victory fanfare
queueSound(10);  // "player"
queueSound(1);   // "one"
queueSound(23);  // "wins"
queueSound(22);  // "game over"
```

---

## APPENDIX C: NeoPixel Modes

5 rings × 12 LEDs = 60 total. Rings: 0,1 = P1,P2; 2 = center; 3,4 = P3,P4.

```cpp
enum NeoMode : uint8_t {
  NEO_OFF = 0,           // All LEDs off
  NEO_IDLE_RAINBOW,      // Rainbow animation (attract screen)
  NEO_STATUS,            // Player rings show join status, center = rainbow
  NEO_RANDOM_FAST,       // Rapid random colors (watch for signal!)
  NEO_FIXED_COLOR,       // All rings same color (green = GO!)
  NEO_COUNTDOWN          // Red blinking (countdown phase)
};
```

### Ring Colors by State
| State | Rings 0,1,3,4 (Players) | Ring 2 (Center) |
|-------|-------------------------|-----------------|
| IDLE | Rainbow | Rainbow |
| JOINING | Yellow (waiting) / Green (joined) | Rainbow |
| COUNTDOWN | Red blink | Red blink |
| REACTION_WAIT | Random fast | Random fast |
| REACTION_ACTIVE | Green (GO!) | Green |
| SHAKE_ACTIVE | Random fast | Random fast |
| RESULTS | Green (winner) / Yellow (other) / Red (penalty) | Rainbow |
| FINAL | Rainbow | Rainbow |

### Coordination with Display
The display should match NeoPixel feedback:
- **Green ring** = show green indicator on screen
- **Red ring** = show red/penalty indicator
- **Random LEDs** = show "Watch LEDs..." text
- **Fixed green** = show "GO!" / "PRESS NOW!"

---

## APPENDIX D: Timing Constants

```cpp
// State durations
#define TIMEOUT_JOIN      30000   // 30s max join phase
#define TIMEOUT_REACTION  10000   // 10s after GO signal
#define TIMEOUT_SHAKE     30000   // 30s max shake phase

// Reaction delays (random selection)
const uint16_t REACT_DELAYS[] = {10000, 15000, 20000};  // 10-20 seconds

// Shake targets (random selection)
const uint8_t SHAKE_TARGETS[] = {10, 15, 20};

// Game structure
#define MAX_PLAYERS       4
#define TOTAL_ROUNDS      5

// State timing in game flow
// IDLE:           3 seconds
// JOINING:        Until 2+ players + 5s idle, or 30s timeout
// COUNTDOWN:      4 seconds (1s mode + 3×1s countdown)
// REACTION_WAIT:  10-20 seconds (random)
// REACTION_ACTIVE: Until all done or 10s timeout
// SHAKE_ACTIVE:   Until all done or 30s timeout
// RESULTS:        5 seconds
// FINAL:          10 seconds
```

---

## APPENDIX E: Accessibility Sync

Every game event triggers THREE feedback channels simultaneously:

| Event | Visual (Display) | Visual (NeoPixel) | Audio | Haptic |
|-------|------------------|-------------------|-------|--------|
| Player joins | Show P# green | Ring green | "Player X ready" | 200ms vibrate |
| Countdown 3 | Show "3" | Red blink | "three" | 150ms pulse |
| Countdown 2 | Show "2" | Red blink | "two" | 150ms pulse |
| Countdown 1 | Show "1" | Red blink | "one" | 150ms pulse |
| GO signal | Show "GO!" | All green | Beep | 300ms vibrate |
| Penalty | Show red/--- | Ring red | Error tone | Double-buzz |
| Round winner | Highlight winner | Ring green | "Player X fastest" | - |
| Final winner | Trophy screen | Rainbow | Fanfare + "wins" | - |

### Display Developer Notes
- When game sends audio, display should show matching visual
- Sync timing: audio plays, display updates, NeoPixel changes - all same frame
- For deaf users: display MUST show all critical info (don't rely on audio alone)
- For blind users: NeoPixel colors match audio cues (green=good, red=bad)

---

## APPENDIX F: Quick Reference Card

```
STATES:          IDLE → JOINING → COUNTDOWN → GAME → RESULTS → (repeat 5x) → FINAL

PLAYER RINGS:    Ring 0 = P1, Ring 1 = P2, Ring 2 = Center, Ring 3 = P3, Ring 4 = P4

TIME VALUES:     0-65534 = valid ms, 0xFFFF (65535) = timeout/penalty

GAME MODES:      0x01 = Reaction (press when green), 0x02 = Shake (reach target count)

MUTEX PATTERN:   ui_update([]{ /* LVGL code here */ });

COLOR CODES:     Green = success/ready, Red = penalty/timeout, Yellow = waiting, Rainbow = idle
```

---

*End of AI Handoff Document - Reaction Reimagined v2.0*
