/*
 * host_test.cpp - ESP32 Host - Full Game Logic
 *
 * Hardware: ESP32 DevKit-C (ZY-ESP32)
 * MAC: 88:57:21:B3:05:AC
 *
 * Pins:
 *   GPIO4:  NeoPixel DIN (5 rings x 12 LEDs = 60)
 *   GPIO18: CC1 / RST - reset all joysticks
 *   GPIO25: I2S DOUT
 *   GPIO26: I2S BCLK
 *   GPIO27: I2S LRC
 *
 * Communication:
 *   Joysticks <-> Host : ESP-NOW (wireless)
 *   Host     -> Display: ESP-NOW (wireless)
 *
 * Game flow (5 rounds):
 *   IDLE -> JOIN -> COUNTDOWN -> REACTION/SHAKE -> COLLECT -> SHOW_RESULTS -> loop
 *   After 5 rounds: FINAL_WINNER -> IDLE
 *
 * NeoPixel ring layout: [P1][P2][Center][P3][P4]
 *   Ring 0 = Player 1
 *   Ring 1 = Player 2
 *   Ring 2 = Center (decorative)
 *   Ring 3 = Player 3
 *   Ring 4 = Player 4
 */

#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>
#include "Protocol.h"
#include "GameTypes.h"
#include "AudioManager.h"

// =============================================================================
// PIN DEFINITIONS
// =============================================================================
#define PIN_NEOPIXEL      4
// #define PIN_RST           18    // CC1 -> reset joysticks

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
Adafruit_NeoPixel pixels(NEOPIXEL_COUNT, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
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

// Join phase: slot-to-joystick mapping (which joystick claimed which player slot)
// slotToStick[slot] = joystick ID (ID_STICK1-4), or 0xFF if unclaimed
uint8_t slotToStick[MAX_PLAYERS] = {0xFF, 0xFF, 0xFF, 0xFF};
uint8_t stickClaimed[MAX_PLAYERS] = {0, 0, 0, 0};  // which joysticks have already claimed a slot
uint8_t currentPromptSlot = 0;      // which player slot we're currently prompting (0-3)
unsigned long promptStartTime = 0;  // when current prompt started

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
// Per-ring color overrides (0 = use default animation). Set during COLLECT/RESULTS.
uint32_t ringOverride[5] = {0,0,0,0,0};
bool ringBlink[5] = {false,false,false,false,false};  // true = blink this ring on/off

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
  for (int i = 0; i < 5; i++) { ringOverride[i] = 0; ringBlink[i] = false; }
}

void resetRound() {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    players[i].finished = false;
    players[i].reactionTime = 0xFFFF;
  }
  for (int i = 0; i < 5; i++) { ringOverride[i] = 0; ringBlink[i] = false; }
}

uint8_t findRoundWinner() {
  uint8_t winner = 0xFF;
  uint16_t best = 0xFFFF;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (players[i].joined && players[i].finished && players[i].reactionTime < best) {
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
// NEOPIXEL FUNCTIONS
// =============================================================================
void setRingColor(uint8_t ring, uint32_t color) {
  uint8_t start = ring * LEDS_PER_RING;
  for (uint8_t i = 0; i < LEDS_PER_RING; i++)
    pixels.setPixelColor(start + i, color);
}

uint32_t wheel(uint8_t pos) {
  pos = 255 - pos;
  if (pos < 85)  return pixels.Color(255 - pos*3, 0, pos*3);
  if (pos < 170) { pos -= 85; return pixels.Color(0, pos*3, 255 - pos*3); }
  pos -= 170;
  return pixels.Color(pos*3, 255 - pos*3, 0);
}

void updateNeoPixels() {
  unsigned long now = millis();

  // Countdown flash overlay: shows bright white on all rings, synced with audio/vibration
  if (countdownFlashStart > 0 && (now - countdownFlashStart) < COUNTDOWN_FLASH_DURATION) {
    // Flash active - set all rings to bright white
    for (int i = 0; i < NEOPIXEL_COUNT; i++) {
      pixels.setPixelColor(i, pixels.Color(255, 255, 255));  // bright white
    }
    pixels.show();
    return;  // skip normal animation during flash
  } else if (countdownFlashStart > 0 && (now - countdownFlashStart) >= COUNTDOWN_FLASH_DURATION) {
    // Flash ended - reset flag and let normal animation take over
    countdownFlashStart = 0;
  }

  switch (neoState) {
    case NEO_IDLE_RAINBOW:
      if (now - neoLastUpdate > 50) {
        neoLastUpdate = now;
        for (int i = 0; i < NEOPIXEL_COUNT; i++)
          pixels.setPixelColor(i, wheel((i * 256 / NEOPIXEL_COUNT + neoOffset) & 255));
        neoOffset++;
        pixels.show();
      }
      break;

    case NEO_RANDOM_FAST:
      // Rainbow cycling effect during reaction timer wait
      // Each ring gets a different hue offset, cycling smoothly
      if (now - neoLastUpdate > 30) {  // ~33 FPS for smooth animation
        neoLastUpdate = now;
        for (int r = 0; r < NUM_RINGS; r++) {
          // Each ring has a different base hue, offset by ring position
          uint8_t ringHue = (neoOffset + r * 51) & 255;  // 51 = 255/5, spread evenly
          uint32_t ringColor = wheel(ringHue);
          setRingColor(r, ringColor);
        }
        neoOffset += 3;  // Speed of rainbow cycling
        pixels.show();
      }
      break;

    case NEO_FIXED_COLOR:
      // Do nothing - last frame stays. Show is already done when we froze.
      break;

    case NEO_COUNTDOWN:
      if (now - neoLastUpdate > 250) {
        neoLastUpdate = now;
        neoBlink = !neoBlink;
        uint32_t c = neoBlink ? pixels.Color(255, 0, 0) : 0;
        for (int i = 0; i < NEOPIXEL_COUNT; i++)
          pixels.setPixelColor(i, c);
        pixels.show();
      }
      break;

    case NEO_STATUS: {
      // Show ring overrides where set, blink rings with ringBlink flag
      if (now - neoLastUpdate > 50) {  // 20 FPS max
        neoLastUpdate = now;
        // Toggle blink state every ~300ms
        bool blinkOn = ((now / 300) % 2) == 0;
        for (int r = 0; r < 5; r++) {
          if (ringOverride[r]) {
            if (ringBlink[r] && !blinkOn)
              setRingColor(r, 0);  // blink off phase
            else
              setRingColor(r, ringOverride[r]);
          } else {
            setRingColor(r, 0);
          }
        }
        pixels.show();
      }
      break;
    }

    case NEO_BLINK_SLOT:
      // Blink the prompted player's ring, show green for already-joined players
      if (now - neoLastUpdate > 300) {
        neoLastUpdate = now;
        neoBlink = !neoBlink;

        // First show all ring overrides (green for joined players)
        for (int r = 0; r < 5; r++) {
          if (ringOverride[r])
            setRingColor(r, ringOverride[r]);
          else
            setRingColor(r, 0);
        }

        // Then blink the current prompt slot's ring (yellow when on)
        uint8_t blinkRing = playerToRing(blinkSlot);
        if (neoBlink) {
          setRingColor(blinkRing, pixels.Color(255, 255, 0));  // yellow blink
        } else {
          setRingColor(blinkRing, 0);  // off
        }
        pixels.show();
      }
      break;

    case NEO_SHAKE_COUNTDOWN:
      // Shake mode: rainbow colors on player rings, center ring countdown
      if (now - neoLastUpdate > 30) {  // ~33 FPS for smooth animation
        neoLastUpdate = now;

        // Player rings: rainbow cycling (or green/red if finished)
        for (int r = 0; r < NUM_RINGS; r++) {
          if (r == CENTER_RING) continue;  // skip center ring
          if (ringOverride[r]) {
            setRingColor(r, ringOverride[r]);  // green/red if finished
          } else {
            // Rainbow color for active players, each ring offset by hue
            uint8_t ringHue = (neoOffset + r * 51) & 255;  // 51 = 255/5, spread evenly
            uint32_t ringColor = wheel(ringHue);
            setRingColor(r, ringColor);
          }
        }
        neoOffset += 3;  // Speed of rainbow cycling

        // Center ring: countdown based on elapsed time
        unsigned long elapsed = now - shakeStartTime;
        uint8_t ledsRemaining = LEDS_PER_RING - (elapsed / SHAKE_LED_INTERVAL);
        if (ledsRemaining > LEDS_PER_RING) ledsRemaining = 0;  // overflow protection

        // Calculate color: green -> yellow -> red as time runs out
        uint8_t r_val, g_val;
        if (ledsRemaining > 8) {
          r_val = 0; g_val = 255;  // green (>66%)
        } else if (ledsRemaining > 4) {
          r_val = 255; g_val = 255;  // yellow (33-66%)
        } else {
          r_val = 255; g_val = 0;  // red (<33%)
        }
        uint32_t countdownColor = pixels.Color(r_val, g_val, 0);

        // Light up remaining LEDs on center ring
        int startIdx = CENTER_RING * LEDS_PER_RING;
        for (int i = 0; i < LEDS_PER_RING; i++) {
          if (i < ledsRemaining) {
            pixels.setPixelColor(startIdx + i, countdownColor);
          } else {
            pixels.setPixelColor(startIdx + i, 0);  // off
          }
        }

        pixels.show();
      }
      break;

    default:
      break;
  }
}

// Set all NeoPixels to yellow when GO fires in reaction mode (visual "press now" cue)
void freezeNeoPixels() {
  // Set all rings to yellow
  for (int r = 0; r < NUM_RINGS; r++) {
    setRingColor(r, pixels.Color(255, 255, 0));  // yellow
  }
  pixels.show();
  neoState = NEO_FIXED_COLOR;
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
// HARDWARE SIGNALS
// =============================================================================
void sendGO() {
  // Send GO signal via ESP-NOW broadcast to all joysticks
  espnowBroadcast(CMD_GO, 0);
  Serial.println("[GO] Sent CMD_GO via ESP-NOW");
}

// void pulseRST() {
//   digitalWrite(PIN_RST, HIGH);
//   delay(10);
//   digitalWrite(PIN_RST, LOW);
// }

// =============================================================================
// ESP-NOW CALLBACKS
// =============================================================================
void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (len != (int)sizeof(GamePacket)) return;

  GamePacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (!validatePacket(&pkt)) return;

  uint8_t src = pkt.src_id;
  if (src < ID_STICK1 || src > ID_STICK4) return;
  uint8_t stickIdx = src - ID_STICK1;  // which physical joystick (0-3)

  uint16_t val = packetData(&pkt);

  // Debug: log all incoming packets
  Serial.printf("[ESP-NOW] Recv cmd=0x%02X from stick %d (0x%02X), data=%d, state=%d\n",
                pkt.cmd, stickIdx + 1, src, val, gameState);

  // Handle join request during JOIN phase
  if (pkt.cmd == CMD_REQ_ID) {
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

      // Turn this slot's ring GREEN
      uint8_t ring = playerToRing(slot);
      ringOverride[ring] = pixels.Color(0, 255, 0);

      // Notify display that player is ready
      sendToDisplay(DISP_PLAYER_READY, 1, slot + 1);

      // Send ACK to joystick (with slot number in data)
      espnowSend((uint8_t*)mac, src, CMD_OK, slot + 1);

      Serial.printf("[JOIN] Joystick %d claimed Player %d slot! Total: %d\n",
                    stickIdx + 1, slot + 1, joinedCount);

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
      ringOverride[ring] = pixels.Color(255, 0, 0);
      ringBlink[ring] = true;  // blink red for penalty
      Serial.printf("[NEO] Player %d ring %d -> BLINK RED (penalty)\n", playerSlot + 1, ring);
    } else {
      ringOverride[ring] = pixels.Color(0, 255, 0);
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
            if (ringOverride[rj] == 0) {
              ringOverride[rj] = pixels.Color(255, 255, 0);
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
    for (int i = 0; i < 5; i++) ringOverride[i] = 0;

    espnowBroadcast(CMD_IDLE, 0);
    sendToDisplay(DISP_IDLE, 0, 0);
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
  sendToDisplay(DISP_PLAYER_PROMPT, 0, slot + 1);

  // Interrupt any currently playing audio, then play new player number
  audio.stop();
  audio.playPlayerNumber(slot + 1);

  Serial.printf("[JOIN] Prompting Player %d slot...\n", slot + 1);
}

void handleJoin() {
  if (stateStartTime == 0) {
    stateStartTime = millis();
    // Clear all ring overrides
    for (int i = 0; i < 5; i++) ringOverride[i] = 0;

    // Start with Player 1 prompt
    startPromptSlot(0);
    Serial.println("[STATE] JOIN - sequential player prompting");
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
        Serial.println("[JOIN] Not enough players, back to IDLE");
        audio.queueSound(SND_ERROR_TONE);
        gameState = STATE_IDLE;
        stateStartTime = 0;
        return;
      }
      Serial.printf("[JOIN] Starting with %d players. Map: [0x%02X,0x%02X,0x%02X,0x%02X]\n",
                    joinedCount, slotToStick[0], slotToStick[1], slotToStick[2], slotToStick[3]);
      audio.queueSound(SND_GET_READY);
      gameState = STATE_COUNTDOWN;
      stateStartTime = 0;
      countdownNum = 3;
      return;
    }

    // Prompt next slot
    startPromptSlot(nextSlot);
  }

  // If all 4 players joined, proceed immediately
  if (joinedCount >= MAX_PLAYERS) {
    Serial.printf("[JOIN] All %d players joined! Map: [0x%02X,0x%02X,0x%02X,0x%02X]\n",
                  joinedCount, slotToStick[0], slotToStick[1], slotToStick[2], slotToStick[3]);
    audio.queueSound(SND_GET_READY);
    gameState = STATE_COUNTDOWN;
    stateStartTime = 0;
    countdownNum = 3;
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
      sendToDisplay(DISP_REACTION_MODE, 0, 0);
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
      espnowBroadcast(CMD_GAME_START, ((uint16_t)gameMode << 8) | 0);

      // Reaction mode: NO countdown - wait for announcements then random delay
      Serial.println("[REACTION] Waiting for announcements before random delay");
      reactionAnnouncementDone = false;
      gameState = STATE_REACTION;
      stateStartTime = 0;
      return;
    } else {
      gameMode = MODE_SHAKE;
      targetIdx = getRandomIndex(&lastTargetIdx, NUM_SHAKE_TARGETS);
      Serial.printf("[COUNTDOWN] Round %d: SHAKE, target=%d\n",
                    currentRound, SHAKE_TARGETS[targetIdx]);
      sendToDisplay(DISP_SHAKE_MODE, 0, SHAKE_TARGETS[targetIdx]);
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
      espnowBroadcast(CMD_GAME_START, ((uint16_t)gameMode << 8) | param);

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
      sendToDisplay(DISP_COUNTDOWN, 0, countdownNum);
      espnowBroadcast(CMD_COUNTDOWN, countdownNum);
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
      sendToDisplay(DISP_COUNTDOWN, 0, countdownNum);
      espnowBroadcast(CMD_COUNTDOWN, countdownNum);
      countdownFlashStart = millis();  // trigger NeoPixel flash sync with audio/vibe
      audio.playCountdown(countdownNum);
      Serial.printf("[COUNTDOWN] %d\n", countdownNum);
      countdownNum--;
    } else {
      // Countdown done -> fire GO for shake mode
      sendToDisplay(DISP_GO, 0, 0);
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
      sendToDisplay(DISP_GO, 0, 0); // display shows "GO" meaning "get ready, watch LEDs"
      Serial.printf("[REACTION] Announcements done, random delay=%dms\n", REACT_DELAYS[delayIdx]);
    }
    return;
  }

  // After random delay, fire GO
  if (millis() - stateStartTime >= REACT_DELAYS[delayIdx]) {
    // FREEZE neopixels - this is the visual "press now" cue
    freezeNeoPixels();

    // Hardware GO pulse to all joysticks (starts their timer + vibration)
    sendGO();

    // Audio beep
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
    // Mark any unfinished players as penalty
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (players[i].joined && !players[i].finished) {
        players[i].finished = true;
        players[i].reactionTime = TIME_PENALTY;
        uint8_t ring = playerToRing(i);
        ringOverride[ring] = pixels.Color(255, 0, 0);
      }
    }
    neoState = NEO_STATUS;
    gameState = STATE_SHOW_RESULTS;
    stateStartTime = 0;
    return;
  }

  // Check if all joined players finished
  bool allDone = true;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (players[i].joined && !players[i].finished) {
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

  // Yellow warning phase for timed-out players: yellow for 5s then blink red
  if (collectYellowPhase) {
    if (millis() - collectYellowStart > TIMEOUT_REACTION) {
      // Warning over - turn timed-out players' rings to blinking red
      for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].joined && players[i].reactionTime == TIME_PENALTY) {
          uint8_t ring = playerToRing(i);
          ringOverride[ring] = pixels.Color(255, 0, 0);
          ringBlink[ring] = true;
        }
      }
      neoState = NEO_STATUS;
      gameState = STATE_SHOW_RESULTS;
      stateStartTime = 0;
      collectYellowPhase = false;
      Serial.println("[COLLECT] Yellow warning done - disqualified");
    }
    return;
  }

  // Check if all joined players have finished (results come in via ESP-NOW callback)
  bool allDone = true;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (players[i].joined && !players[i].finished) {
      allDone = false;
      break;
    }
  }

  // Timeout after TIMEOUT_REACTION
  bool timedOut = false;
  if (!allDone && millis() - stateStartTime > TIMEOUT_REACTION) {
    // Mark remaining players as penalty with yellow rings (warning)
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (players[i].joined && !players[i].finished) {
        players[i].finished = true;
        players[i].reactionTime = TIME_PENALTY;
        uint8_t ring = playerToRing(i);
        ringOverride[ring] = pixels.Color(255, 255, 0);
        ringBlink[ring] = false;  // solid yellow during warning
        Serial.printf("[COLLECT] Player %d: TIMEOUT -> yellow warning\n", i + 1);
      }
    }
    timedOut = true;
    allDone = true;
  }

  if (allDone) {
    if (timedOut) {
      // Some players timed out - start yellow warning phase (5s)
      collectYellowPhase = true;
      collectYellowStart = millis();
      neoState = NEO_STATUS;
      Serial.println("[COLLECT] Starting yellow warning phase (5s)");
    } else {
      // All responded before timeout (early-press penalties already blink red)
      neoState = NEO_STATUS;
      gameState = STATE_SHOW_RESULTS;
      stateStartTime = 0;
    }
  }
}

// --- SHOW RESULTS ---
static bool resultsPhase2 = false;  // false = showing times, true = showing winner/scores

void handleShowResults() {
  if (stateStartTime == 0) {
    stateStartTime = millis();
    resultsPhase2 = false;

    // Phase 1: Send all reaction times to display
    const uint8_t timeCmds[] = {DISP_TIME_P1, DISP_TIME_P2, DISP_TIME_P3, DISP_TIME_P4};
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (players[i].joined) {
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
      sendToDisplay(DISP_ROUND_WINNER, 0, winner + 1);
      audio.playPlayerNumber(winner + 1);
      audio.queueSound(SND_FASTEST);
      Serial.printf("[RESULTS] Round %d winner: Player %d\n", currentRound, winner+1);
    } else {
      sendToDisplay(DISP_ROUND_WINNER, 0, 0); // no winner
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
    if (currentRound >= TOTAL_ROUNDS) {
      gameState = STATE_FINAL_WINNER;
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
      sendToDisplay(DISP_FINAL_WINNER, 0, winner + 1);
      // Play winner announcement first, then victory music, then game over
      audio.playPlayerWins(winner + 1);
      audio.queueSound(SND_VICTORY_FANFARE);
    } else {
      Serial.println("[FINAL] No winner (all scores 0)");
      sendToDisplay(DISP_FINAL_WINNER, 0, 0);
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
  Serial.println("\n=== REACTION REIMAGINED - HOST ===");

  // // Hardware control pins
  // pinMode(PIN_RST, OUTPUT);
  // digitalWrite(PIN_RST, LOW);

  // NeoPixels
  pixels.begin();
  pixels.setBrightness(NEO_BRIGHTNESS);
  pixels.show();

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
