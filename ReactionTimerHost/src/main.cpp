/*
 * Reaction Time Duel - ESP32 Host - Full Game Logic
 *
 * Hardware: ESP32 DevKit-C (ZY-ESP32)
 * MAC: 88:57:21:B3:05:AC
 *
 * Pins:
 *   GPIO4:  NeoPixel DIN (5 rings x 12 LEDs = 60)
 *   GPIO16: WS2812B Strip DIN (89 LEDs, ambient animations)
 *   GPIO25: I2S DOUT
 *   GPIO26: I2S BCLK
 *   GPIO27: I2S LRC
 *   GPIO33: Amplifier GAIN (PWM volume control)
 *
 * Communication:
 *   Joysticks <-> Host : ESP-NOW (wireless)
 *   Host     -> Display: ESP-NOW (wireless)
 *
 * Game flow (5 rounds):
 *   IDLE -> JOIN -> COUNTDOWN -> REACTION/SHAKE -> COLLECT -> SHOW_RESULTS -> loop
 *   After 5 rounds: FINAL_WINNER -> IDLE
 *
 * NeoPixel ring layout (left-to-right): [P1][P2][Center][P3][P4]
 *   Player 1 = Ring 4
 *   Player 2 = Ring 3
 *   Center   = Ring 2 (decorative / shake countdown)
 *   Player 3 = Ring 1
 *   Player 4 = Ring 0
 *
 */

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <NeoPixelBrightnessBus.h>
#include "Protocol.h"
#include "GameTypes.h"
#include "AudioManager.h"

// =============================================================================
// PIN DEFINITIONS
// =============================================================================
#define PIN_NEOPIXEL      4
#define PIN_STRIP         16    // WS2812B ambient light strip
#define STRIP_LED_COUNT   89
#define STRIP_BRIGHTNESS  80

// =============================================================================
// ESP-NOW MAC ADDRESSES
// =============================================================================
uint8_t displayMac[6]  = {0xD0, 0xCF, 0x13, 0x01, 0xD1, 0xA4};
uint8_t stick1Mac[6]   = {0xBC, 0xFF, 0x4D, 0xF9, 0xF3, 0x91};
uint8_t stick2Mac[6]   = {0xBC, 0xFF, 0x4D, 0xF9, 0xAE, 0x29};
uint8_t stick3Mac[6]   = {0xBC, 0xFF, 0x4D, 0xF9, 0xAC, 0x42};
uint8_t stick4Mac[6]   = {0xBC, 0xFF, 0x4D, 0xF9, 0xBE, 0x62};
uint8_t broadcastMac[6]= {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// =============================================================================
// HARDWARE
// =============================================================================
// Both use ESP32 RMT with DMA — Show() is non-blocking (no interrupt disable)
NeoPixelBrightnessBus<NeoGrbFeature, NeoEsp32Rmt0800KbpsMethod> pixels(NEOPIXEL_COUNT, PIN_NEOPIXEL);
NeoPixelBrightnessBus<NeoGrbFeature, NeoEsp32Rmt1800KbpsMethod> strip(STRIP_LED_COUNT, PIN_STRIP);
AudioManager audio;

// =============================================================================
// GAME STATE
// =============================================================================
enum HostGameState : uint8_t {
  STATE_IDLE,
  STATE_JOIN,
  STATE_COUNTDOWN,
  STATE_REACTION,       // waiting for random delay, then fires GO
  STATE_SHAKE,          // GO already fired, waiting for shake results
  STATE_COLLECT,        // polling joysticks for results
  STATE_SHOW_RESULTS,
  STATE_FINAL_WINNER
};

HostGameState gameState = STATE_IDLE;
unsigned long stateStartTime = 0;

// Players
Player players[MAX_PLAYERS];
uint8_t joinedCount = 0;
uint8_t currentRound = 0;

// Deuce mode
bool inDeuce = false;
uint8_t deucePlayer[2] = {0xFF, 0xFF};  // indices (0-3) of the two tied players

// Join phase: slot-to-joystick mapping (which joystick claimed which player slot)
// slotToStick[slot] = joystick ID (ID_STICK1-4), or 0xFF if unclaimed
uint8_t slotToStick[MAX_PLAYERS] = {0xFF, 0xFF, 0xFF, 0xFF};
uint8_t stickClaimed[MAX_PLAYERS] = {0, 0, 0, 0};  // which joysticks have already claimed a slot
uint8_t currentPromptSlot = 0;      // which player slot we're currently prompting (0-3)
unsigned long promptStartTime = 0;  // when current prompt started
bool joinComplete = false;          // true = enough players, waiting 1s before countdown
unsigned long joinCompleteTime = 0; // when join completed (for 1s color-display delay)

// Round config
uint8_t gameMode = MODE_REACTION;   // current round mode
uint8_t delayIdx = 0;               // index into REACT_DELAYS
uint8_t targetIdx = 0;              // index into SHAKE_TARGETS
uint8_t lastDelayIdx  = 0xFF;       // prevent repeat
uint8_t lastTargetIdx = 0xFF;

// Mode shuffle bag: ensures both modes are played before repeating
uint8_t modeBag[2] = {MODE_REACTION, MODE_SHAKE};
uint8_t modeBagIdx = 2;  // start at 2 to trigger reshuffle on first use

// First-time instruction tracking (only show instructions first time each mode is played)
bool reactionInstructPlayed = false;
bool shakeInstructPlayed = false;

// Reaction announcement tracking
bool reactionAnnouncementDone = false;  // wait for voice before random delay
bool reactionFirstInstruct = false;     // true when instructions were queued this round

// Shake countdown tracking
unsigned long shakeStartTime = 0;   // for center ring countdown
uint8_t shakeProgress[MAX_PLAYERS] = {0,0,0,0};  // current shake count per player (from CMD_SHAKE_PROGRESS)
uint8_t shakeTargetCount = 0;       // current round's shake target (10/15/20)

// Countdown
uint8_t countdownNum = 3;
unsigned long countdownFlashStart = 0;  // for sync flash on countdown
#define COUNTDOWN_FLASH_DURATION 200    // ms - matches slave vibration duration
bool shakeAnnouncementDone = false;     // tracks if shake announcement phase is complete
bool shakeFirstInstruct = false;       // true when shake instructions were queued this round
// Shake announcement delays (based on audio file lengths + 250ms gaps)
// Subsequent: get_ready(0.99s) + gap + shake(0.82s) + gap + target(≤1.27s) = ~3.6s
#define SHAKE_ANNOUNCE_DELAY        4000    // ms - subsequent rounds
// First time: + will_shake(4.30s) + gap = ~8.1s
#define SHAKE_ANNOUNCE_DELAY_FIRST  8500    // ms - first round with instructions

// Collect phase
uint8_t collectPlayer = 0;          // which player we're waiting on next
unsigned long collectTimeout = 0;
bool collectYellowPhase = false;     // yellow warning before disqualification
unsigned long collectYellowStart = 0;

// =============================================================================
// NEOPIXEL STATE (uses NeoMode from GameTypes.h)
// =============================================================================
// Mapping: NEO_IDLE_RAINBOW, NEO_RANDOM_FAST, NEO_FIXED_COLOR, NEO_COUNTDOWN, NEO_STATUS, NEO_BLINK_SLOT
NeoMode neoState = NEO_IDLE_RAINBOW;
uint32_t neoOffset = 0;
unsigned long neoLastUpdate = 0;
bool neoBlink = false;
uint8_t blinkSlot = 0;  // which player slot to blink (0-3) for NEO_BLINK_SLOT mode
// Per-ring color overrides (black = use default animation). Set during COLLECT/RESULTS.
RgbColor ringOverride[5];  // default-constructs to black (0,0,0)
bool ringBlink[5] = {false,false,false,false,false};  // true = blink this ring on/off
static const RgbColor RGB_OFF(0);
static const RgbColor RGB_RED(255, 0, 0);
static const RgbColor RGB_GREEN(0, 255, 0);
static const RgbColor RGB_BLUE(0, 0, 255);
static const RgbColor RGB_YELLOW(255, 255, 0);
static const RgbColor RGB_WHITE(255, 255, 255);

// Joystick identity colors (by joystick ID: 1=White, 2=Blue, 3=Red, 4=Yellow)
RgbColor stickColor(uint8_t stickId) {
  switch (stickId) {
    case ID_STICK1: return RGB_WHITE;
    case ID_STICK2: return RGB_BLUE;
    case ID_STICK3: return RGB_RED;
    case ID_STICK4: return RGB_YELLOW;
    default:        return RGB_GREEN;
  }
}

// =============================================================================
// HELPERS (playerToRing is in GameTypes.h)
// =============================================================================
uint8_t getRandomIndex(uint8_t *last, uint8_t max) {
  uint8_t idx;
  do { idx = random(max); } while (idx == *last && max > 1);
  *last = idx;
  return idx;
}

// Shuffle bag for game modes: ensures both modes are played before repeating
uint8_t getNextGameMode() {
  if (modeBagIdx >= 2) {
    // Reshuffle: swap randomly
    modeBagIdx = 0;
    if (random(2) == 0) {
      uint8_t tmp = modeBag[0];
      modeBag[0] = modeBag[1];
      modeBag[1] = tmp;
    }
    Serial.printf("[MODE] Reshuffled bag: [%s, %s]\n",
                  modeBag[0] == MODE_REACTION ? "REACT" : "SHAKE",
                  modeBag[1] == MODE_REACTION ? "REACT" : "SHAKE");
  }
  return modeBag[modeBagIdx++];
}

// Is this player active in the current round? (filters for deuce)
bool isActivePlayer(uint8_t i) {
  if (!players[i].joined || slotToStick[i] == 0xFF) return false;
  if (inDeuce) return (i == deucePlayer[0] || i == deucePlayer[1]);
  return true;
}

// Check if exactly 2 players share the highest score (deuce condition)
bool checkDeuce() {
  uint8_t maxScore = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (players[i].joined && players[i].score > maxScore)
      maxScore = players[i].score;
  }
  uint8_t count = 0;
  uint8_t tied[2] = {0xFF, 0xFF};
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (players[i].joined && players[i].score == maxScore) {
      if (count < 2) tied[count] = i;
      count++;
    }
  }
  if (count == 2) {
    deucePlayer[0] = tied[0];
    deucePlayer[1] = tied[1];
    return true;
  }
  return false;
}

// Forward declaration for ACK tracking (full implementation below ESP-NOW send section)
#define ACK_MAX_RETRIES    3
#define ACK_RETRY_INTERVAL 50   // ms
#define ACK_SLOT_COUNT     5    // 4 joysticks + 1 display

struct PendingAck {
  bool waiting;
  uint8_t dest;
  uint8_t cmd;
  uint16_t data;
  uint8_t mac[6];
  uint8_t retries;
  unsigned long lastSend;
};

PendingAck pendingAcks[ACK_SLOT_COUNT];

void resetPlayers() {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    players[i].joined = false;
    players[i].finished = false;
    players[i].reactionTime = 0xFFFF;
    players[i].score = 0;
    slotToStick[i] = 0xFF;
    stickClaimed[i] = 0;
  }
  joinedCount = 0;
  currentPromptSlot = 0;
  modeBagIdx = 2;  // reset shuffle bag for new game
  reactionInstructPlayed = false;  // reset instruction flag for new game
  shakeInstructPlayed = false;
  inDeuce = false;
  deucePlayer[0] = 0xFF;
  deucePlayer[1] = 0xFF;
  for (int i = 0; i < 5; i++) { ringOverride[i] = RGB_OFF; ringBlink[i] = false; }
  for (int i = 0; i < ACK_SLOT_COUNT; i++) pendingAcks[i].waiting = false;
}

void resetRound() {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    players[i].finished = false;
    players[i].reactionTime = 0xFFFF;
    shakeProgress[i] = 0;
  }
  for (int i = 0; i < 5; i++) { ringOverride[i] = RGB_OFF; ringBlink[i] = false; }
}

uint8_t findRoundWinner() {
  uint8_t winner = 0xFF;
  uint16_t best = 0xFFFF;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (isActivePlayer(i) && players[i].finished && players[i].reactionTime < best) {
      best = players[i].reactionTime;
      winner = i;
    }
  }
  return winner;
}

uint8_t findFinalWinner() {
  uint8_t winner = 0xFF;
  uint8_t best = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (players[i].joined && players[i].score > best) {
      best = players[i].score;
      winner = i;
    }
  }
  return winner;
}

// =============================================================================
// NEOPIXEL FUNCTIONS (NeoPixelBus — non-blocking RMT DMA)
// =============================================================================
void setRingColor(uint8_t ring, RgbColor color) {
  uint8_t start = ring * LEDS_PER_RING;
  for (uint8_t i = 0; i < LEDS_PER_RING; i++)
    pixels.SetPixelColor(start + i, color);
}

RgbColor wheel(uint8_t pos) {
  pos = 255 - pos;
  if (pos < 85)  return RgbColor(255 - pos*3, 0, pos*3);
  if (pos < 170) { pos -= 85; return RgbColor(0, pos*3, 255 - pos*3); }
  pos -= 170;
  return RgbColor(pos*3, 255 - pos*3, 0);
}

// Non-blocking show for game rings
void pixelsShow() {
  if (pixels.CanShow()) pixels.Show();
}

void updateNeoPixels() {
  unsigned long now = millis();

  // Countdown flash overlay: shows bright white on all rings, synced with audio/vibration
  if (countdownFlashStart > 0 && (now - countdownFlashStart) < COUNTDOWN_FLASH_DURATION) {
    pixels.ClearTo(RGB_WHITE);
    pixelsShow();
    return;
  } else if (countdownFlashStart > 0 && (now - countdownFlashStart) >= COUNTDOWN_FLASH_DURATION) {
    countdownFlashStart = 0;
  }

  switch (neoState) {
    case NEO_IDLE_RAINBOW:
      if (now - neoLastUpdate > 50) {
        neoLastUpdate = now;
        for (int i = 0; i < NEOPIXEL_COUNT; i++)
          pixels.SetPixelColor(i, wheel((i * 256 / NEOPIXEL_COUNT + neoOffset) & 255));
        neoOffset++;
        pixelsShow();
      }
      break;

    case NEO_RANDOM_FAST:
      if (now - neoLastUpdate > 30) {
        neoLastUpdate = now;
        for (int r = 0; r < NUM_RINGS; r++) {
          uint8_t ringHue = (neoOffset + r * 51) & 255;
          setRingColor(r, wheel(ringHue));
        }
        neoOffset += 3;
        pixelsShow();
      }
      break;

    case NEO_FIXED_COLOR:
      // Re-render frozen yellow until pixels actually update on screen
      // (handles case where initial Show() was skipped due to RMT DMA busy)
      if (now - neoLastUpdate > 50) {
        neoLastUpdate = now;
        pixels.ClearTo(RGB_OFF);
        for (int i = 0; i < MAX_PLAYERS; i++) {
          if (players[i].joined) {
            setRingColor(playerToRing(i), RGB_YELLOW);
          }
        }
        pixelsShow();
      }
      break;

    case NEO_COUNTDOWN:
      if (now - neoLastUpdate > 250) {
        neoLastUpdate = now;
        neoBlink = !neoBlink;
        RgbColor c = neoBlink ? RGB_RED : RGB_OFF;
        pixels.ClearTo(c);
        pixelsShow();
      }
      break;

    case NEO_STATUS: {
      if (now - neoLastUpdate > 50) {
        neoLastUpdate = now;
        bool blinkOn = ((now / 300) % 2) == 0;
        for (int r = 0; r < 5; r++) {
          if (ringOverride[r] != RGB_OFF) {
            if (ringBlink[r] && !blinkOn)
              setRingColor(r, RGB_OFF);
            else
              setRingColor(r, ringOverride[r]);
          } else {
            setRingColor(r, RGB_OFF);
          }
        }
        pixelsShow();
      }
      break;
    }

    case NEO_BLINK_SLOT:
      if (now - neoLastUpdate > 300) {
        neoLastUpdate = now;
        neoBlink = !neoBlink;

        for (int r = 0; r < 5; r++) {
          if (ringOverride[r] != RGB_OFF)
            setRingColor(r, ringOverride[r]);
          else
            setRingColor(r, RGB_OFF);
        }

        uint8_t blinkRing = playerToRing(blinkSlot);
        setRingColor(blinkRing, neoBlink ? RGB_GREEN : RGB_OFF);
        pixelsShow();
      }
      break;

    case NEO_SHAKE_COUNTDOWN:
      if (now - neoLastUpdate > 30) {
        neoLastUpdate = now;

        // Player rings: show green progress bar based on shake count
        for (int r = 0; r < NUM_RINGS; r++) {
          if (r == CENTER_RING) continue;
          if (ringOverride[r] != RGB_OFF) {
            // Player finished: solid green or blinking red (handled by ringOverride)
            if (ringBlink[r] && !((now / 300) % 2))
              setRingColor(r, RGB_OFF);
            else
              setRingColor(r, ringOverride[r]);
          } else {
            // Find which player maps to this ring and show progress
            int8_t player = -1;
            for (int p = 0; p < MAX_PLAYERS; p++) {
              if (players[p].joined && playerToRing(p) == r) {
                player = p;
                break;
              }
            }
            if (player >= 0 && shakeTargetCount > 0) {
              // White background with green progress overlay
              uint8_t ledsLit = (uint8_t)((uint16_t)shakeProgress[player] * LEDS_PER_RING / shakeTargetCount);
              if (ledsLit > LEDS_PER_RING) ledsLit = LEDS_PER_RING;
              int startIdx = r * LEDS_PER_RING;
              for (int i = 0; i < LEDS_PER_RING; i++) {
                pixels.SetPixelColor(startIdx + i, (i < ledsLit) ? RGB_GREEN : RGB_WHITE);
              }
            } else {
              setRingColor(r, RGB_OFF);
            }
          }
        }

        unsigned long elapsed = now - shakeStartTime;
        uint8_t ledsRemaining = LEDS_PER_RING - (elapsed / SHAKE_LED_INTERVAL);
        if (ledsRemaining > LEDS_PER_RING) ledsRemaining = 0;

        RgbColor countdownColor;
        if (ledsRemaining > 8)       countdownColor = RGB_GREEN;
        else if (ledsRemaining > 4)  countdownColor = RGB_YELLOW;
        else                          countdownColor = RGB_RED;

        int startIdx = CENTER_RING * LEDS_PER_RING;
        for (int i = 0; i < LEDS_PER_RING; i++) {
          pixels.SetPixelColor(startIdx + i, (i < ledsRemaining) ? countdownColor : RGB_OFF);
        }

        pixelsShow();
      }
      break;

    default:
      break;
  }
}

// Set NeoPixels to yellow for joined players when GO fires (visual "press now" cue)
void freezeNeoPixels() {
  pixels.ClearTo(RGB_OFF);
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (players[i].joined) {
      setRingColor(playerToRing(i), RGB_YELLOW);
    }
  }
  pixelsShow();
  neoState = NEO_FIXED_COLOR;
}

// =============================================================================
// WS2812B STRIP - NON-BLOCKING RANDOM ANIMATIONS (89 LEDs on GPIO16)
// Uses NeoPixelBus with ESP32 RMT DMA — Show() returns immediately.
// =============================================================================
enum StripAnim : uint8_t {
  ANIM_RAINBOW_CYCLE,
  ANIM_SPARKLE,
  ANIM_METEOR,
  ANIM_COLOR_CHASE,
  ANIM_BREATHING,
  ANIM_FIRE,
  ANIM_COUNT  // total number of animations
};

StripAnim stripAnim = ANIM_RAINBOW_CYCLE;
unsigned long stripLastUpdate = 0;
unsigned long stripAnimStart = 0;       // when current animation started
uint32_t stripStep = 0;                 // animation step counter
#define STRIP_ANIM_DURATION  15000      // switch animation every 15 seconds

// Per-LED heat buffer for fire effect
uint8_t stripHeat[STRIP_LED_COUNT];

// Helper: color wheel for the strip (returns RgbColor)
RgbColor stripWheel(uint8_t pos) {
  pos = 255 - pos;
  if (pos < 85)  return RgbColor(255 - pos * 3, 0, pos * 3);
  if (pos < 170) { pos -= 85; return RgbColor(0, pos * 3, 255 - pos * 3); }
  pos -= 170;
  return RgbColor(pos * 3, 255 - pos * 3, 0);
}

// Helper: dim an RgbColor by a factor (0-255)
RgbColor dimColor(RgbColor color, uint8_t bright) {
  return RgbColor(
    (uint16_t)color.R * bright / 255,
    (uint16_t)color.G * bright / 255,
    (uint16_t)color.B * bright / 255
  );
}

// Helper: non-blocking show — only pushes data if RMT DMA is idle
void stripShow() {
  if (strip.CanShow()) strip.Show();
}

// --- Animation: Rainbow Cycle ---
void stripRainbowCycle() {
  if (millis() - stripLastUpdate < 30) return;
  stripLastUpdate = millis();
  for (int i = 0; i < STRIP_LED_COUNT; i++) {
    strip.SetPixelColor(i, stripWheel((i * 256 / STRIP_LED_COUNT + stripStep) & 255));
  }
  stripShow();
  stripStep++;
}

// --- Animation: Sparkle / Twinkle ---
void stripSparkle() {
  if (millis() - stripLastUpdate < 50) return;
  stripLastUpdate = millis();
  // Fade all LEDs slightly
  for (int i = 0; i < STRIP_LED_COUNT; i++) {
    RgbColor c = strip.GetPixelColor(i);
    strip.SetPixelColor(i, dimColor(c, 200));
  }
  // Light up 2-3 random LEDs with random bright colors
  for (int s = 0; s < 3; s++) {
    int pos = random(STRIP_LED_COUNT);
    strip.SetPixelColor(pos, stripWheel(random(256)));
  }
  stripShow();
}

// --- Animation: Meteor Rain ---
void stripMeteor() {
  if (millis() - stripLastUpdate < 25) return;
  stripLastUpdate = millis();
  // Randomly fade each LED (creates trail)
  for (int i = 0; i < STRIP_LED_COUNT; i++) {
    if (random(10) > 4) {
      RgbColor c = strip.GetPixelColor(i);
      strip.SetPixelColor(i, dimColor(c, 160));
    }
  }
  // Draw meteor head (6 LEDs)
  uint16_t head = stripStep % (STRIP_LED_COUNT + 20);
  for (int j = 0; j < 6; j++) {
    int pos = head - j;
    if (pos >= 0 && pos < STRIP_LED_COUNT) {
      uint8_t bright = 255 - j * 40;
      strip.SetPixelColor(pos, dimColor(RgbColor(200, 80, 255), bright));
    }
  }
  stripShow();
  stripStep++;
}

// --- Animation: Color Chase ---
void stripColorChase() {
  if (millis() - stripLastUpdate < 60) return;
  stripLastUpdate = millis();
  // 3 colored segments chasing around the strip
  for (int i = 0; i < STRIP_LED_COUNT; i++) {
    uint8_t seg = (i + stripStep) % 18;
    if (seg < 6)       strip.SetPixelColor(i, RgbColor(255, 0, 0));
    else if (seg < 12) strip.SetPixelColor(i, RgbColor(0, 255, 0));
    else               strip.SetPixelColor(i, RgbColor(0, 0, 255));
  }
  stripShow();
  stripStep++;
}

// --- Animation: Breathing (single color pulsing) ---
void stripBreathing() {
  if (millis() - stripLastUpdate < 20) return;
  stripLastUpdate = millis();
  uint8_t phase = stripStep & 0xFF;
  // Triangle wave: 0->255->0
  uint8_t level = (phase < 128) ? phase * 2 : (255 - phase) * 2;
  // Gamma-correct for smoother visual
  uint8_t bright = (uint16_t)level * level / 255;
  // Cycle hue slowly over time
  uint8_t hue = (stripStep / 4) & 0xFF;
  RgbColor color = dimColor(stripWheel(hue), bright);
  strip.ClearTo(color);
  stripShow();
  stripStep++;
}

// --- Animation: Fire Effect ---
void stripFire() {
  if (millis() - stripLastUpdate < 30) return;
  stripLastUpdate = millis();
  // Cool down every cell a little
  for (int i = 0; i < STRIP_LED_COUNT; i++) {
    uint8_t cooldown = random(0, 20);
    stripHeat[i] = (stripHeat[i] > cooldown) ? stripHeat[i] - cooldown : 0;
  }
  // Heat drifts up and diffuses
  for (int i = STRIP_LED_COUNT - 1; i >= 2; i--) {
    stripHeat[i] = (stripHeat[i - 1] + stripHeat[i - 2] + stripHeat[i - 2]) / 3;
  }
  // Randomly ignite new sparks near the bottom
  if (random(255) < 160) {
    int pos = random(7);
    stripHeat[pos] = min(255L, (long)stripHeat[pos] + random(160, 255));
  }
  // Map heat to color (black -> red -> yellow -> white)
  for (int i = 0; i < STRIP_LED_COUNT; i++) {
    uint8_t t = stripHeat[i];
    uint8_t r, g, b;
    if (t < 85) {
      r = t * 3; g = 0; b = 0;
    } else if (t < 170) {
      r = 255; g = (t - 85) * 3; b = 0;
    } else {
      r = 255; g = 255; b = (t - 170) * 3;
    }
    strip.SetPixelColor(i, RgbColor(r, g, b));
  }
  stripShow();
}

// --- Strip Update: picks and runs current animation, switches randomly ---
void updateStrip() {
  unsigned long now = millis();
  // Switch to a random animation periodically
  if (now - stripAnimStart > STRIP_ANIM_DURATION) {
    StripAnim newAnim;
    do { newAnim = (StripAnim)random(ANIM_COUNT); } while (newAnim == stripAnim && ANIM_COUNT > 1);
    stripAnim = newAnim;
    stripStep = 0;
    stripAnimStart = now;
    memset(stripHeat, 0, sizeof(stripHeat));
    // Clear strip on transition for clean start
    strip.ClearTo(RgbColor(0));
    stripShow();
    Serial.printf("[STRIP] Switched to animation %d\n", stripAnim);
  }
  switch (stripAnim) {
    case ANIM_RAINBOW_CYCLE: stripRainbowCycle(); break;
    case ANIM_SPARKLE:       stripSparkle();      break;
    case ANIM_METEOR:        stripMeteor();       break;
    case ANIM_COLOR_CHASE:   stripColorChase();   break;
    case ANIM_BREATHING:     stripBreathing();    break;
    case ANIM_FIRE:          stripFire();         break;
    default:                 stripRainbowCycle(); break;
  }
}

// =============================================================================
// ESP-NOW -> DISPLAY
// =============================================================================
void sendToDisplay(uint8_t cmd, uint8_t dataHigh, uint8_t dataLow) {
  GamePacket pkt;
  uint16_t data = ((uint16_t)dataHigh << 8) | dataLow;
  buildPacket(&pkt, ID_DISPLAY, ID_HOST, cmd, data);
  esp_now_send(displayMac, (uint8_t*)&pkt, sizeof(pkt));
  Serial.printf("[DISP] cmd=0x%02X data=%d,%d\n", cmd, dataHigh, dataLow);
}

// =============================================================================
// ESP-NOW SEND
// =============================================================================
void espnowSend(uint8_t* mac, uint8_t dest, uint8_t cmd, uint16_t data) {
  GamePacket pkt;
  buildPacket(&pkt, dest, ID_HOST, cmd, data);
  esp_now_send(mac, (uint8_t*)&pkt, sizeof(pkt));
}

void espnowBroadcast(uint8_t cmd, uint16_t data) {
  espnowSend(broadcastMac, ID_BROADCAST, cmd, data);
}

// =============================================================================
// ACK + RETRY (non-blocking)
// PendingAck struct, pendingAcks[], and defines declared above resetPlayers()
// =============================================================================

// Map stick/display ID to pending ACK slot index (0-3 = sticks, 4 = display)
int8_t ackSlotFor(uint8_t destId) {
  if (destId >= ID_STICK1 && destId <= ID_STICK4) return destId - ID_STICK1;
  if (destId == ID_DISPLAY) return 4;
  return -1;
}

// MAC lookup by device ID
uint8_t* macForDest(uint8_t destId) {
  switch (destId) {
    case ID_STICK1: return stick1Mac;
    case ID_STICK2: return stick2Mac;
    case ID_STICK3: return stick3Mac;
    case ID_STICK4: return stick4Mac;
    case ID_DISPLAY: return displayMac;
    default: return nullptr;
  }
}

// Send a critical command with ACK tracking (replaces espnowSend for critical cmds)
void sendWithRetry(uint8_t destId, uint8_t cmd, uint16_t data) {
  int8_t slot = ackSlotFor(destId);
  if (slot < 0) return;

  uint8_t* mac = macForDest(destId);
  if (!mac) return;

  // Store pending ACK
  PendingAck &pa = pendingAcks[slot];
  pa.waiting = true;
  pa.dest = destId;
  pa.cmd = cmd;
  pa.data = data;
  memcpy(pa.mac, mac, 6);
  pa.retries = ACK_MAX_RETRIES;
  pa.lastSend = millis();

  // Send first attempt
  espnowSend(mac, destId, cmd, data);
  Serial.printf("[ACK] Sent cmd=0x%02X to 0x%02X (retries=%d)\n", cmd, destId, pa.retries);
}

// Send a critical command to all active joysticks (respects deuce filtering)
void sendToJoysticksWithRetry(uint8_t cmd, uint16_t data) {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (isActivePlayer(i)) {
      sendWithRetry(slotToStick[i], cmd, data);
    }
  }
}

// Send a critical command to display with ACK tracking
void sendToDisplayWithRetry(uint8_t cmd, uint8_t dataHigh, uint8_t dataLow) {
  uint16_t data = ((uint16_t)dataHigh << 8) | dataLow;
  sendWithRetry(ID_DISPLAY, cmd, data);
  Serial.printf("[DISP] cmd=0x%02X data=%d,%d\n", cmd, dataHigh, dataLow);
}

// Called in loop() — retransmits pending commands that haven't been ACK'd
void updateRetries() {
  unsigned long now = millis();
  for (int i = 0; i < ACK_SLOT_COUNT; i++) {
    PendingAck &pa = pendingAcks[i];
    if (!pa.waiting) continue;
    if (now - pa.lastSend < ACK_RETRY_INTERVAL) continue;

    if (pa.retries > 0) {
      pa.retries--;
      pa.lastSend = now;
      espnowSend(pa.mac, pa.dest, pa.cmd, pa.data);
      Serial.printf("[ACK] Retry cmd=0x%02X to 0x%02X (retries=%d)\n", pa.cmd, pa.dest, pa.retries);
    } else {
      pa.waiting = false;
      Serial.printf("[ACK] GAVE UP cmd=0x%02X to 0x%02X\n", pa.cmd, pa.dest);
    }
  }
}

// Handle incoming ACK — clear matching pending slot
void handleAck(uint8_t srcId, uint8_t ackedCmd) {
  int8_t slot = ackSlotFor(srcId);
  if (slot < 0) return;

  PendingAck &pa = pendingAcks[slot];
  if (pa.waiting && pa.cmd == ackedCmd) {
    pa.waiting = false;
    Serial.printf("[ACK] Received ACK for cmd=0x%02X from 0x%02X\n", ackedCmd, srcId);
  }
}

// =============================================================================
// HARDWARE SIGNALS
// =============================================================================
void sendGO() {
  // Send GO individually to each joined joystick (with ACK tracking)
  sendToJoysticksWithRetry(CMD_GO, 0);
  Serial.println("[GO] Sent CMD_GO to all joined joysticks");
}

// =============================================================================
// ESP-NOW CALLBACKS
// =============================================================================
void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len != (int)sizeof(GamePacket)) return;

  GamePacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (!validatePacket(&pkt)) return;

  uint8_t src = pkt.src_id;
  uint16_t val = packetData(&pkt);

  // Handle CMD_ACK from any source (joysticks or display)
  if (pkt.cmd == CMD_ACK) {
    handleAck(src, pkt.data_low);
    return;
  }

  // All other commands must come from joysticks
  if (src < ID_STICK1 || src > ID_STICK4) return;
  uint8_t stickIdx = src - ID_STICK1;  // which physical joystick (0-3)

  // Debug: log all incoming packets
  Serial.printf("[ESP-NOW] Recv cmd=0x%02X from stick %d (0x%02X), data=%d, state=%d\n",
                pkt.cmd, stickIdx + 1, src, val, gameState);

  // Handle join request during JOIN phase
  if (pkt.cmd == CMD_REQ_ID) {
    // Decode joystick firmware version from data field
    // data_high = (MAJOR<<4)|MINOR, data_low = PATCH
    uint8_t jsMajor = (pkt.data_high >> 4) & 0x0F;
    uint8_t jsMinor = pkt.data_high & 0x0F;
    uint8_t jsPatch = pkt.data_low;
    Serial.printf("[JOIN] Joystick %d firmware: V%d.%d.%d\n",
                  stickIdx + 1, jsMajor, jsMinor, jsPatch);

    if (gameState == STATE_JOIN) {
      // Check if this joystick already claimed a slot
      if (stickClaimed[stickIdx]) {
        Serial.printf("[JOIN] Joystick %d already claimed a slot, ignoring\n", stickIdx + 1);
        return;
      }

      // Check if current prompted slot is still available
      uint8_t slot = currentPromptSlot;
      if (slotToStick[slot] != 0xFF) {
        Serial.printf("[JOIN] Slot %d already taken, ignoring\n", slot + 1);
        return;
      }

      // Claim the current slot for this joystick
      slotToStick[slot] = src;
      stickClaimed[stickIdx] = 1;
      players[slot].joined = true;
      joinedCount++;

      // Turn this slot's ring to the joystick's identity color
      uint8_t ring = playerToRing(slot);
      ringOverride[ring] = stickColor(src);

      // Notify display that player is ready (data_high = player slot 1-4, data_low = joystick ID)
      sendToDisplayWithRetry(DISP_PLAYER_READY, slot + 1, src);

      // Send ACK to joystick (with slot number in data)
      espnowSend((uint8_t*)mac, src, CMD_OK, slot + 1);

      Serial.printf("[JOIN] Joystick %d (V%d.%d.%d) claimed Player %d slot! Total: %d\n",
                    stickIdx + 1, jsMajor, jsMinor, jsPatch, slot + 1, joinedCount);

      // Advance to next slot immediately
      promptStartTime = 0;  // trigger immediate advance in handleJoin
    }
    return;
  }

  // Find which player slot this joystick is assigned to
  int8_t playerSlot = -1;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (slotToStick[i] == src) {
      playerSlot = i;
      break;
    }
  }
  if (playerSlot < 0) {
    Serial.printf("[ERR] Joystick 0x%02X not in slotToStick! Map: [0x%02X,0x%02X,0x%02X,0x%02X]\n",
                  src, slotToStick[0], slotToStick[1], slotToStick[2], slotToStick[3]);
    return;
  }

  // Handle shake progress updates (data_high = count, data_low = target)
  if (pkt.cmd == CMD_SHAKE_PROGRESS) {
    if (playerSlot >= 0 && playerSlot < MAX_PLAYERS && gameState == STATE_SHAKE) {
      shakeProgress[playerSlot] = pkt.data_high;
      Serial.printf("[SHAKE] Player %d progress: %d/%d\n",
                    playerSlot + 1, pkt.data_high, pkt.data_low);
    }
    return;
  }

  if (pkt.cmd == CMD_REACTION_DONE || pkt.cmd == CMD_SHAKE_DONE) {
    // Check we're in correct state to receive results
    if (gameState != STATE_COLLECT && gameState != STATE_SHAKE) {
      Serial.printf("[WARN] Got result in wrong state %d, ignoring\n", gameState);
      return;
    }
    if (players[playerSlot].finished) {
      Serial.printf("[WARN] Player %d already finished, ignoring\n", playerSlot + 1);
      return;
    }

    players[playerSlot].reactionTime = val;
    players[playerSlot].finished = true;

    Serial.printf("[RECV] Player %d (stick %d): %s = %d ms\n",
                  playerSlot + 1, stickIdx + 1,
                  (pkt.cmd == CMD_REACTION_DONE) ? "REACTION" : "SHAKE",
                  val);

    // Immediately turn that player's ring GREEN if valid, BLINK RED if penalty
    uint8_t ring = playerToRing(playerSlot);
    if (val == TIME_PENALTY) {
      ringOverride[ring] = RGB_RED;
      ringBlink[ring] = true;  // blink red for penalty
      Serial.printf("[NEO] Player %d ring %d -> BLINK RED (penalty)\n", playerSlot + 1, ring);
    } else {
      ringOverride[ring] = RGB_GREEN;
      ringBlink[ring] = false;
      Serial.printf("[NEO] Player %d ring %d -> GREEN (time=%d)\n", playerSlot + 1, ring, val);
    }
    // If we were in NEO_FIXED_COLOR or NEO_RANDOM_FAST, switch to status mode
    // so the ring override renders immediately
    if (neoState == NEO_FIXED_COLOR || neoState == NEO_RANDOM_FAST) {
      // During COLLECT: set non-finished players' rings to yellow so they stay visible
      if (gameState == STATE_COLLECT) {
        for (int j = 0; j < MAX_PLAYERS; j++) {
          if (players[j].joined && !players[j].finished) {
            uint8_t rj = playerToRing(j);
            if (ringOverride[rj] == RGB_OFF) {
              ringOverride[rj] = RGB_YELLOW;
              ringBlink[rj] = false;
            }
          }
        }
      }
      neoState = NEO_STATUS;
    }
  }
}

void OnDataSent(const uint8_t *mac, esp_now_send_status_t status) {
  // optional
}

// =============================================================================
// STATE HANDLERS
// =============================================================================

// --- IDLE ---
void handleIdle() {
  if (stateStartTime == 0) {
    stateStartTime = millis();
    resetPlayers();
    currentRound = 0;
    neoState = NEO_IDLE_RAINBOW;
    neoOffset = 0;
    for (int i = 0; i < 5; i++) ringOverride[i] = RGB_OFF;

    sendToJoysticksWithRetry(CMD_IDLE, 0);
    sendToDisplayWithRetry(DISP_IDLE, 0, 0);
    audio.queueSound(SND_PRESS_TO_JOIN);
    Serial.println("[STATE] IDLE");
  }

  // Auto-transition to JOIN after 3s
  if (millis() - stateStartTime > 3000) {
    gameState = STATE_JOIN;
    stateStartTime = 0;
  }
}

// --- JOIN ---
// Sequential player slot prompting: P1 -> P2 -> P3 -> P4
// Each slot blinks on NeoPixel ring and display. Any unclaimed joystick can press to claim.
// After all 4 prompts (5s each) or skip timeout, proceed if >= 2 players joined.
#define PROMPT_DURATION 5000  // 5 seconds per player prompt

void startPromptSlot(uint8_t slot) {
  currentPromptSlot = slot;
  promptStartTime = millis();
  blinkSlot = slot;
  neoState = NEO_BLINK_SLOT;
  neoBlink = false;
  neoLastUpdate = 0;

  // Send prompt to display (player number 1-4)
  sendToDisplayWithRetry(DISP_PLAYER_PROMPT, 0, slot + 1);

  // Interrupt any currently playing audio, then play new player number
  audio.stop();
  audio.playPlayerNumber(slot + 1);

  Serial.printf("[JOIN] Prompting Player %d slot...\n", slot + 1);
}

void handleJoin() {
  if (stateStartTime == 0) {
    stateStartTime = millis();
    joinComplete = false;
    // Clear all ring overrides
    for (int i = 0; i < 5; i++) ringOverride[i] = RGB_OFF;

    // Start with Player 1 prompt
    startPromptSlot(0);
    Serial.println("[STATE] JOIN - sequential player prompting");
  }

  // Waiting 1s after join complete so players can see their assigned colors
  if (joinComplete) {
    if (millis() - joinCompleteTime > 1000) {
      audio.queueSound(SND_GET_READY);
      gameState = STATE_COUNTDOWN;
      stateStartTime = 0;
      countdownNum = 3;
    }
    return;
  }

  // Check if current slot has timed out or was just claimed (promptStartTime = 0)
  bool shouldAdvance = (promptStartTime == 0) ||
                       (millis() - promptStartTime > PROMPT_DURATION);

  if (shouldAdvance) {
    // Find next unclaimed slot
    uint8_t nextSlot = currentPromptSlot + 1;
    while (nextSlot < MAX_PLAYERS && slotToStick[nextSlot] != 0xFF) {
      nextSlot++;  // skip already-claimed slots
    }

    if (nextSlot >= MAX_PLAYERS) {
      // All slots prompted - check if we have enough players
      if (joinedCount < 2) {
        Serial.println("[JOIN] Not enough players, restarting join prompts");
        // Silently cycle back to P1 without error sound or idle screen
        currentPromptSlot = 0;
        startPromptSlot(0);
        return;
      }
      Serial.printf("[JOIN] Starting with %d players. Map: [0x%02X,0x%02X,0x%02X,0x%02X]\n",
                    joinedCount, slotToStick[0], slotToStick[1], slotToStick[2], slotToStick[3]);
      // Show all joined player colors for 1s before starting
      neoState = NEO_STATUS;
      audio.stop();
      joinComplete = true;
      joinCompleteTime = millis();
      return;
    }

    // Prompt next slot
    startPromptSlot(nextSlot);
  }

  // If all 4 players joined, show colors for 1s before starting
  if (joinedCount >= MAX_PLAYERS) {
    Serial.printf("[JOIN] All %d players joined! Map: [0x%02X,0x%02X,0x%02X,0x%02X]\n",
                  joinedCount, slotToStick[0], slotToStick[1], slotToStick[2], slotToStick[3]);
    neoState = NEO_STATUS;
    audio.stop();
    joinComplete = true;
    joinCompleteTime = millis();
  }
}

// --- COUNTDOWN ---
void handleCountdown() {
  if (stateStartTime == 0) {
    stateStartTime = millis();
    resetRound();
    currentRound++;

    // Pick mode from shuffle bag (ensures both modes played before repeat)
    gameMode = getNextGameMode();
    if (gameMode == MODE_REACTION) {
      delayIdx = getRandomIndex(&lastDelayIdx, NUM_REACT_DELAYS);
      Serial.printf("[COUNTDOWN] Round %d: REACTION, delay=%dms\n",
                    currentRound, REACT_DELAYS[delayIdx]);
      sendToDisplayWithRetry(DISP_REACTION_MODE, 0, 0);
      audio.queueSound(SND_REACTION_MODE);
      // Only play instruction the first time reaction mode is played this game
      if (!reactionInstructPlayed) {
        audio.queueSound(SND_REACTION_INSTRUCT);
        reactionInstructPlayed = true;
        reactionFirstInstruct = true;
        Serial.println("[COUNTDOWN] First reaction mode - playing instruction");
      } else {
        reactionFirstInstruct = false;
      }
      // NeoPixels: random cycling during reaction mode
      neoState = NEO_RANDOM_FAST;

      // Send mode to joysticks
      sendToJoysticksWithRetry(CMD_GAME_START, ((uint16_t)gameMode << 8) | 0);

      // Reaction mode: NO countdown - wait for announcements then random delay
      Serial.println("[REACTION] Waiting for announcements before random delay");
      reactionAnnouncementDone = false;
      gameState = STATE_REACTION;
      stateStartTime = 0;
      return;
    } else {
      gameMode = MODE_SHAKE;
      targetIdx = getRandomIndex(&lastTargetIdx, NUM_SHAKE_TARGETS);
      shakeTargetCount = SHAKE_TARGETS[targetIdx];
      Serial.printf("[COUNTDOWN] Round %d: SHAKE, target=%d\n",
                    currentRound, SHAKE_TARGETS[targetIdx]);
      sendToDisplayWithRetry(DISP_SHAKE_MODE, 0, SHAKE_TARGETS[targetIdx]);
      audio.queueSound(SND_SHAKE_IT);
      // Only play instruction the first time shake mode is played this game
      if (!shakeInstructPlayed) {
        audio.queueSound(SND_YOU_WILL_SHAKE);
        shakeInstructPlayed = true;
        shakeFirstInstruct = true;
        Serial.println("[COUNTDOWN] First shake mode - playing instruction");
      } else {
        shakeFirstInstruct = false;
      }
      // Announce target number (plays "Ten", "Fifteen", or "Twenty")
      audio.playShakeTarget(SHAKE_TARGETS[targetIdx]);
      // NeoPixels: red blink during countdown for shake mode
      neoState = NEO_COUNTDOWN;

      // Send mode+param to all joysticks so they know what to do after GO
      uint16_t param = SHAKE_TARGETS[targetIdx];
      sendToJoysticksWithRetry(CMD_GAME_START, ((uint16_t)gameMode << 8) | param);

      // Wait for announcements to finish before starting countdown
      shakeAnnouncementDone = false;
      countdownNum = 3;
    }
  }

  // Shake mode: wait for announcements before starting countdown
  if (gameMode == MODE_SHAKE && !shakeAnnouncementDone) {
    unsigned long shakeAnnounceDelay = shakeFirstInstruct
        ? SHAKE_ANNOUNCE_DELAY_FIRST
        : SHAKE_ANNOUNCE_DELAY;
    if (millis() - stateStartTime > shakeAnnounceDelay) {
      shakeAnnouncementDone = true;
      stateStartTime = millis();  // reset for countdown timing
      // Start countdown 3
      sendToDisplayWithRetry(DISP_COUNTDOWN, 0, countdownNum);
      sendToJoysticksWithRetry(CMD_COUNTDOWN, countdownNum);
      countdownFlashStart = millis();
      audio.playCountdown(countdownNum);
      Serial.printf("[COUNTDOWN] %d\n", countdownNum);
      countdownNum--;
    }
    return;  // wait for announcement delay
  }

  // Tick every 1 second (countdown 2, 1, GO)
  if (millis() - stateStartTime > 1000) {
    stateStartTime = millis();

    // Countdown only runs for shake mode (reaction mode skips directly to STATE_REACTION)
    if (countdownNum > 0) {
      sendToDisplayWithRetry(DISP_COUNTDOWN, 0, countdownNum);
      sendToJoysticksWithRetry(CMD_COUNTDOWN, countdownNum);
      countdownFlashStart = millis();  // trigger NeoPixel flash sync with audio/vibe
      audio.playCountdown(countdownNum);
      Serial.printf("[COUNTDOWN] %d\n", countdownNum);
      countdownNum--;
    } else {
      // Countdown done -> fire GO for shake mode
      sendToDisplayWithRetry(DISP_GO, 0, 0);
      sendGO(); // hardware sync - joysticks vibrate on hardware GO
      audio.queueSound(SND_BEEP);
      Serial.println("[GO] Shake mode started!");
      gameState = STATE_SHAKE;
      stateStartTime = 0;
    }
  }
}

// --- REACTION (random wait then GO) ---
// Waits for voice announcements, then random delay, then GO
void handleReaction() {
  if (stateStartTime == 0) {
    stateStartTime = millis();
    // NeoPixels should already be in NEO_RANDOM_FAST
    // (for reaction mode). If not, set it.
    if (neoState != NEO_RANDOM_FAST) neoState = NEO_RANDOM_FAST;
    Serial.println("[REACTION] Waiting for announcements...");
  }

  // Wait for voice announcements to finish before starting random delay
  if (!reactionAnnouncementDone) {
    unsigned long announceDelay = reactionFirstInstruct
        ? REACT_ANNOUNCE_DELAY_FIRST
        : REACT_ANNOUNCE_DELAY;
    if (millis() - stateStartTime > announceDelay) {
      reactionAnnouncementDone = true;
      stateStartTime = millis();  // reset for random delay timing
      Serial.printf("[REACTION] Announcements done, random delay=%dms\n", REACT_DELAYS[delayIdx]);
    }
    return;
  }

  // After random delay, fire GO
  if (millis() - stateStartTime >= REACT_DELAYS[delayIdx]) {
    // FREEZE neopixels - this is the visual "press now" cue
    freezeNeoPixels();

    // Display GO - synced with neopixels and joysticks
    sendToDisplayWithRetry(DISP_GO, 0, 0);

    // Hardware GO pulse to all joysticks (starts their timer + vibration)
    sendGO();

    // Audio beep - stop any pending sounds so beep plays immediately
    audio.stop();
    audio.queueSound(SND_BEEP);

    Serial.println("[GO] Reaction GO fired! LEDs frozen.");

    // Move to collect
    gameState = STATE_COLLECT;
    stateStartTime = 0;
    collectPlayer = 0;
  }
}

// --- SHAKE (GO already fired, wait for shake results) ---
void handleShake() {
  if (stateStartTime == 0) {
    stateStartTime = millis();
    shakeStartTime = millis();  // for center ring countdown
    neoState = NEO_SHAKE_COUNTDOWN;  // random player rings + center countdown
    Serial.println("[SHAKE] Waiting for shake results...");
  }

  // Timeout after 30s
  if (millis() - stateStartTime > TIMEOUT_SHAKE) {
    Serial.println("[SHAKE] Timeout - moving to results");
    // Mark any unfinished active players as penalty
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (isActivePlayer(i) && !players[i].finished) {
        players[i].finished = true;
        players[i].reactionTime = TIME_PENALTY;
        uint8_t ring = playerToRing(i);
        ringOverride[ring] = RGB_RED;
      }
    }
    neoState = NEO_STATUS;
    gameState = STATE_SHOW_RESULTS;
    stateStartTime = 0;
    return;
  }

  // Check if all active players finished
  bool allDone = true;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (isActivePlayer(i) && !players[i].finished) {
      allDone = false;
      break;
    }
  }
  if (allDone) {
    Serial.println("[SHAKE] All players done");
    neoState = NEO_STATUS;
    gameState = STATE_SHOW_RESULTS;
    stateStartTime = 0;
  }
}

// --- COLLECT (reaction mode - wait for all results, with yellow warning) ---
void handleCollect() {
  if (stateStartTime == 0) {
    stateStartTime = millis();
    collectYellowPhase = false;
    Serial.println("[COLLECT] Waiting for reaction results...");
  }

  // Check if all active players have finished (results come in via ESP-NOW callback)
  bool allDone = true;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (isActivePlayer(i) && !players[i].finished) {
      allDone = false;
      break;
    }
  }

  // Yellow warning phase: players can still react during this time
  if (collectYellowPhase) {
    if (allDone || millis() - collectYellowStart > TIMEOUT_REACTION) {
      // Warning expired or all responded - disqualify any still unfinished
      for (int i = 0; i < MAX_PLAYERS; i++) {
        if (isActivePlayer(i) && !players[i].finished) {
          players[i].finished = true;
          players[i].reactionTime = TIME_PENALTY;
          uint8_t ring = playerToRing(i);
          ringOverride[ring] = RGB_RED;
          ringBlink[ring] = true;
        }
      }
      neoState = NEO_STATUS;
      gameState = STATE_SHOW_RESULTS;
      stateStartTime = 0;
      collectYellowPhase = false;
      Serial.println("[COLLECT] Yellow warning done - disqualified remaining");
    }
    return;
  }

  if (allDone) {
    // All responded before timeout (early-press penalties already blink red)
    neoState = NEO_STATUS;
    gameState = STATE_SHOW_RESULTS;
    stateStartTime = 0;
    return;
  }

  // Timeout after TIMEOUT_REACTION - start yellow warning
  // Players are NOT marked as finished yet - they can still react!
  if (millis() - stateStartTime > TIMEOUT_REACTION) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (isActivePlayer(i) && !players[i].finished) {
        uint8_t ring = playerToRing(i);
        ringOverride[ring] = RGB_YELLOW;
        ringBlink[ring] = false;  // solid yellow during warning
        Serial.printf("[COLLECT] Player %d: yellow warning (can still react)\n", i + 1);
      }
    }
    collectYellowPhase = true;
    collectYellowStart = millis();
    neoState = NEO_STATUS;
    Serial.println("[COLLECT] Starting yellow warning phase (5s)");
  }
}

// --- SHOW RESULTS ---
static bool resultsPhase2 = false;  // false = showing times, true = showing winner/scores

void handleShowResults() {
  if (stateStartTime == 0) {
    stateStartTime = millis();
    resultsPhase2 = false;

    // Phase 1: Send reaction times to display (only active players)
    const uint8_t timeCmds[] = {DISP_TIME_P1, DISP_TIME_P2, DISP_TIME_P3, DISP_TIME_P4};
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (isActivePlayer(i)) {
        uint16_t t = players[i].reactionTime;
        sendToDisplay(timeCmds[i], (t >> 8) & 0xFF, t & 0xFF);
        delay(10);
      }
    }
    Serial.println("[RESULTS] Phase 1: Showing reaction times");
  }

  // After 3 seconds, send winner and scores (Phase 2)
  if (!resultsPhase2 && millis() - stateStartTime > 3000) {
    resultsPhase2 = true;

    // Find and announce winner
    uint8_t winner = findRoundWinner();
    if (winner != 0xFF) {
      players[winner].score++;
      sendToDisplayWithRetry(DISP_ROUND_WINNER, 0, winner + 1);
      audio.playPlayerNumber(winner + 1);
      audio.queueSound(SND_FASTEST);
      Serial.printf("[RESULTS] Round %d winner: Player %d\n", currentRound, winner+1);
    } else {
      sendToDisplayWithRetry(DISP_ROUND_WINNER, 0, 0); // no winner
      Serial.println("[RESULTS] No winner this round");
    }

    // Send scores
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (players[i].joined) {
        sendToDisplay(DISP_SCORES, i + 1, players[i].score);
      }
    }

    Serial.println("[RESULTS] Phase 2: Showing winner and scores");
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (players[i].joined)
        Serial.printf("  Player %d: score=%d, time=%d ms\n", i+1, players[i].score, players[i].reactionTime);
    }
  }

  // After 6 seconds total (3s times + 3s scores), transition to next state
  if (millis() - stateStartTime > 6000) {
    if (inDeuce) {
      // Check if either deuce player has enough lead
      int diff = abs((int)players[deucePlayer[0]].score - (int)players[deucePlayer[1]].score);
      if (diff >= DEUCE_LEAD) {
        Serial.printf("[DEUCE] Lead of %d reached - going to final winner\n", diff);
        gameState = STATE_FINAL_WINNER;
      } else {
        Serial.printf("[DEUCE] Score diff=%d, need %d - continuing\n", diff, DEUCE_LEAD);
        gameState = STATE_COUNTDOWN;
      }
    } else if (currentRound >= TOTAL_ROUNDS) {
      if (checkDeuce()) {
        // Deuce detected — enter deuce mode
        inDeuce = true;
        Serial.printf("[DEUCE] Deuce between Player %d and Player %d!\n",
                      deucePlayer[0] + 1, deucePlayer[1] + 1);
        sendToDisplayWithRetry(DISP_DEUCE, deucePlayer[0] + 1, deucePlayer[1] + 1);
        // Send CMD_IDLE to non-deuce joysticks
        for (int i = 0; i < MAX_PLAYERS; i++) {
          if (players[i].joined && slotToStick[i] != 0xFF &&
              i != deucePlayer[0] && i != deucePlayer[1]) {
            sendWithRetry(slotToStick[i], CMD_IDLE, 0);
            Serial.printf("[DEUCE] Sent CMD_IDLE to Player %d (out of deuce)\n", i + 1);
          }
        }
        gameState = STATE_COUNTDOWN;
      } else {
        gameState = STATE_FINAL_WINNER;
      }
    } else {
      gameState = STATE_COUNTDOWN;
    }
    stateStartTime = 0;
    resultsPhase2 = false;
  }
}

// --- FINAL WINNER ---
void handleFinalWinner() {
  if (stateStartTime == 0) {
    stateStartTime = millis();
    uint8_t winner = findFinalWinner();
    neoState = NEO_IDLE_RAINBOW;
    neoOffset = 0;

    if (winner != 0xFF) {
      Serial.printf("[FINAL] Winner: Player %d\n", winner + 1);
      sendToDisplayWithRetry(DISP_FINAL_WINNER, 0, winner + 1);
      // Play winner announcement first, then victory music, then game over
      audio.playPlayerWins(winner + 1);
      audio.queueSound(SND_VICTORY_FANFARE);
    } else {
      Serial.println("[FINAL] No winner (all scores 0)");
      sendToDisplayWithRetry(DISP_FINAL_WINNER, 0, 0);
    }
    audio.queueSound(SND_GAME_OVER);
  }

  if (millis() - stateStartTime > DURATION_FINAL) {
    gameState = STATE_IDLE;
    stateStartTime = 0;
  }
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println("       REACTION TIME DUEL - HOST");
  Serial.printf("            Firmware %s\n", FW_VERSION_STRING);
  Serial.println("========================================");

  // Game rings (NeoPixelBus RMT ch0 — non-blocking)
  pixels.Begin();
  pixels.SetBrightness(NEO_BRIGHTNESS);
  pixels.Show();

  // WS2812B ambient strip (89 LEDs) — NeoPixelBus with RMT DMA (non-blocking)
  strip.Begin();
  strip.SetBrightness(STRIP_BRIGHTNESS);
  strip.Show();
  stripAnimStart = millis();
  Serial.println("WS2812B strip ready (89 LEDs on GPIO16, NeoPixelBus RMT DMA)");

  // Audio
  if (audio.begin()) {
    Serial.println("Audio ready");
  } else {
    Serial.println("Audio init failed - continuing without audio");
  }

  // ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed!");
    return;
  }
  esp_now_register_recv_cb(OnDataRecv);
  esp_now_register_send_cb(OnDataSent);

  // Add peers
  auto addPeer = [](uint8_t* mac, const char* name) {
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = ESPNOW_CHANNEL;
    p.encrypt = false;
    p.ifidx = WIFI_IF_STA;
    if (esp_now_add_peer(&p) == ESP_OK) {
      Serial.printf("Paired: %s\n", name);
    } else {
      Serial.printf("Pair failed: %s\n", name);
    }
  };

  addPeer(broadcastMac, "Broadcast");
  addPeer(displayMac, "Display");
  addPeer(stick1Mac, "Joystick 1");
  addPeer(stick2Mac, "Joystick 2");
  // Only add stick3/4 if MACs are filled (non-zero)
  if (stick3Mac[0] || stick3Mac[1] || stick3Mac[2])
    addPeer(stick3Mac, "Joystick 3");
  if (stick4Mac[0] || stick4Mac[1] || stick4Mac[2])
    addPeer(stick4Mac, "Joystick 4");

  Serial.print("Host MAC: ");
  Serial.println(WiFi.macAddress());

  // Random seed
  randomSeed(analogRead(36));

  // Players join dynamically via CMD_REQ_ID during JOIN phase
  Serial.println("Host ready! Waiting for players to join...");
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
  audio.update();
  updateNeoPixels();
  updateStrip();
  updateRetries();

  switch (gameState) {
    case STATE_IDLE:            handleIdle();           break;
    case STATE_JOIN:            handleJoin();           break;
    case STATE_COUNTDOWN:       handleCountdown();      break;
    case STATE_REACTION:        handleReaction();       break;
    case STATE_SHAKE:           handleShake();          break;
    case STATE_COLLECT:         handleCollect();        break;
    case STATE_SHOW_RESULTS:    handleShowResults();    break;
    case STATE_FINAL_WINNER:    handleFinalWinner();    break;
  }
  // No delay - ESP32 handles WiFi/system tasks automatically via FreeRTOS
}
