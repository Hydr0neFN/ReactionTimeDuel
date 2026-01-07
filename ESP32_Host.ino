/*
 * ESP32_Host.ino - Reaction Time Duel Game Controller
 * 
 * Hardware: ESP32-DEVKIT-V1
 * 
 * Pin Assignments (from schematic):
 *   GPIO17 (TX2): → D+ → All joysticks RX + Display RX
 *   GPIO16 (RX2): ← D- ← All joysticks TX + Display TX (via voltage divider)
 *   GPIO18: CC1 → RST signal to all joysticks
 *   GPIO19: CC2 → GO signal to all joysticks (hardware timing sync!)
 *   GPIO4:  NeoPixel DIN (5 rings × 12 LEDs = 60)
 *   GPIO23: I2S DOUT (DINS to MAX98357A)
 *   GPIO26: I2S BCLK
 *   GPIO25: I2S LRC
 * 
 * NOTE: SD Card pins (18,19,23) conflict with CC1/CC2/DOUT!
 *       Audio requires alternative solution (SPIFFS or different SPI pins)
 * 
 * Core 0: Game state machine, protocol handling
 * Core 1: NeoPixel animation task
 */

#include <Adafruit_NeoPixel.h>
#include "Protocol.h"
#include "AudioManager.h"

// =============================================================================
// PIN DEFINITIONS (from schematic - DO NOT CHANGE, PCB is printed)
// =============================================================================
#define PIN_UART_TX       17    // TX2 → All devices
#define PIN_UART_RX       16    // RX2 ← All devices (via voltage divider)
#define PIN_RST           18    // CC1 → Joystick RST (directly to PB5)
#define PIN_GO            19    // CC2 → Joystick GO (to PB2)
#define PIN_NEOPIXEL      4     // DIN to NeoPixel rings
#define PIN_I2S_DOUT      23    // DINS
#define PIN_I2S_BCLK      26    // BCLK  
#define PIN_I2S_LRC       25    // LRC

// =============================================================================
// CONFIGURATION
// =============================================================================
#define SERIAL_BAUD           9600
#define NEOPIXEL_COUNT        60
#define LEDS_PER_RING         12
#define NEOPIXEL_BRIGHTNESS   50

// =============================================================================
// GAME CONSTANTS
// =============================================================================
#define MAX_PLAYERS           4
#define TOTAL_ROUNDS          5
#define TIMEOUT_JOIN          60000   // 60s join phase
#define TIMEOUT_REACTION      10000   // 10s after GO
#define TIMEOUT_SHAKE         30000   // 30s max
#define TIMEOUT_TOKEN         100     // ms per player response

// Delays and targets
const uint16_t REACT_DELAYS[] = {10000, 15000, 20000};
const uint8_t SHAKE_TARGETS[] = {10, 20, 30};

// =============================================================================
// GAME STATE
// =============================================================================
enum GameState : uint8_t {
  GAME_IDLE,
  GAME_ASSIGN_IDS,
  GAME_COUNTDOWN,
  GAME_REACTION,
  GAME_SHAKE,
  GAME_COLLECT_RESULTS,
  GAME_SHOW_RESULTS,
  GAME_FINAL_WINNER
};

// =============================================================================
// NEOPIXEL MODES
// =============================================================================
enum NeoMode : uint8_t {
  NEO_OFF,
  NEO_IDLE_RAINBOW,
  NEO_STATUS,
  NEO_RANDOM_FAST,
  NEO_FIXED_COLOR,
  NEO_COUNTDOWN_BLINK
};

// =============================================================================
// PLAYER DATA
// =============================================================================
struct Player {
  bool joined;
  bool finished;
  uint16_t reactionTime;
  uint8_t score;
};

// =============================================================================
// GLOBALS
// =============================================================================
Adafruit_NeoPixel pixels(NEOPIXEL_COUNT, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
AudioManager audio;

volatile NeoMode neoMode = NEO_OFF;
volatile uint32_t neoColor = 0;

GameState gameState = GAME_IDLE;
Player players[MAX_PLAYERS];
uint8_t joinedCount = 0;
uint8_t currentRound = 0;
uint8_t currentAssignSlot = 0;
uint8_t gameMode = 0;
uint8_t delayIndex = 0;
uint8_t targetIndex = 0;
unsigned long stateStartTime = 0;

uint8_t rxPacket[PACKET_SIZE];
uint8_t txPacket[PACKET_SIZE];

TaskHandle_t animationTaskHandle;

// Prevent same mode twice
uint8_t lastDelayIdx = 0xFF;
uint8_t lastTargetIdx = 0xFF;

// =============================================================================
// CRC8 (from Protocol.h but inline for clarity)
// =============================================================================
uint8_t calcCRC(uint8_t *data, uint8_t len) {
  uint8_t crc = 0x00;
  while (len--) {
    uint8_t extract = *data++;
    for (uint8_t i = 8; i; i--) {
      uint8_t sum = (crc ^ extract) & 0x01;
      crc >>= 1;
      if (sum) crc ^= 0x8C;
      extract >>= 1;
    }
  }
  return crc;
}

// =============================================================================
// PACKET TRANSMISSION
// =============================================================================
void sendPacket(uint8_t dest, uint8_t cmd, uint8_t dataHigh, uint8_t dataLow) {
  txPacket[0] = PACKET_START;
  txPacket[1] = dest;
  txPacket[2] = ID_HOST;
  txPacket[3] = cmd;
  txPacket[4] = dataHigh;
  txPacket[5] = dataLow;
  
  uint8_t crcData[5] = {dest, ID_HOST, cmd, dataHigh, dataLow};
  txPacket[6] = calcCRC(crcData, 5);
  
  Serial2.write(txPacket, PACKET_SIZE);
  Serial2.flush();
  delay(DELAY_PACKET);
}

bool receivePacket(unsigned long timeout = 0) {
  unsigned long start = millis();
  
  while (Serial2.available() < PACKET_SIZE) {
    if (timeout > 0 && (millis() - start) > timeout) return false;
    delay(1);
  }
  
  while (Serial2.available() >= PACKET_SIZE) {
    if (Serial2.peek() == PACKET_START) {
      for (int i = 0; i < PACKET_SIZE; i++) {
        rxPacket[i] = Serial2.read();
      }
      
      if (rxPacket[1] == ID_HOST) {
        uint8_t crcData[5] = {rxPacket[1], rxPacket[2], rxPacket[3], rxPacket[4], rxPacket[5]};
        if (calcCRC(crcData, 5) == rxPacket[6]) {
          return true;
        }
      }
      return false;
    } else {
      Serial2.read();
    }
  }
  return false;
}

// =============================================================================
// HARDWARE SIGNALS (GO and RST)
// =============================================================================
void pulseGO() {
  // Hardware GO signal for precise timing sync
  digitalWrite(PIN_GO, HIGH);
  delay(50);  // 50ms pulse
  digitalWrite(PIN_GO, LOW);
}

void pulseRST() {
  // Reset all joysticks
  digitalWrite(PIN_RST, HIGH);
  delay(10);
  digitalWrite(PIN_RST, LOW);
}

// =============================================================================
// NEOPIXEL HELPERS
// =============================================================================
void setRingColor(uint8_t ring, uint32_t color) {
  uint8_t start = ring * LEDS_PER_RING;
  for (uint8_t i = 0; i < LEDS_PER_RING; i++) {
    pixels.setPixelColor(start + i, color);
  }
}

void setAllRings(uint32_t color) {
  for (int i = 0; i < NEOPIXEL_COUNT; i++) {
    pixels.setPixelColor(i, color);
  }
}

uint32_t wheel(uint8_t pos) {
  pos = 255 - pos;
  if (pos < 85) return pixels.Color(255 - pos * 3, 0, pos * 3);
  if (pos < 170) { pos -= 85; return pixels.Color(0, pos * 3, 255 - pos * 3); }
  pos -= 170;
  return pixels.Color(pos * 3, 255 - pos * 3, 0);
}

// =============================================================================
// ANIMATION TASK (Core 1)
// =============================================================================
void animationTask(void *param) {
  uint8_t offset = 0;
  unsigned long lastUpdate = 0;
  bool blinkState = false;
  
  while (true) {
    unsigned long now = millis();
    
    switch (neoMode) {
      case NEO_OFF:
        setAllRings(0);
        pixels.show();
        break;
        
      case NEO_IDLE_RAINBOW:
        if (now - lastUpdate > 50) {
          lastUpdate = now;
          for (int i = 0; i < NEOPIXEL_COUNT; i++) {
            pixels.setPixelColor(i, wheel((i * 256 / NEOPIXEL_COUNT + offset) & 255));
          }
          pixels.show();
          offset++;
        }
        break;
        
      case NEO_STATUS:
        // Rings 0,1 = Players 1,2; Ring 2 = center; Rings 3,4 = Players 3,4
        for (uint8_t p = 0; p < MAX_PLAYERS; p++) {
          uint8_t ring = (p < 2) ? p : (p + 1);
          uint32_t color = players[p].joined ? 
            pixels.Color(0, 255, 0) : pixels.Color(255, 255, 0);
          setRingColor(ring, color);
        }
        setRingColor(2, wheel(offset++));  // Center rainbow
        pixels.show();
        break;
        
      case NEO_RANDOM_FAST:
        if (now - lastUpdate > 100) {
          lastUpdate = now;
          for (int i = 0; i < NEOPIXEL_COUNT; i++) {
            pixels.setPixelColor(i, wheel(random(256)));
          }
          pixels.show();
        }
        break;
        
      case NEO_FIXED_COLOR:
        setAllRings(neoColor);
        pixels.show();
        break;
        
      case NEO_COUNTDOWN_BLINK:
        if (now - lastUpdate > 250) {
          lastUpdate = now;
          blinkState = !blinkState;
          setAllRings(blinkState ? pixels.Color(255, 0, 0) : 0);
          pixels.show();
        }
        break;
    }
    
    delay(10);
  }
}

// =============================================================================
// GAME HELPERS
// =============================================================================
void resetPlayers() {
  for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
    players[i] = {false, false, 0xFFFF, 0};
  }
  joinedCount = 0;
}

void resetRound() {
  for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
    players[i].finished = false;
    players[i].reactionTime = 0xFFFF;
  }
}

uint8_t findWinner() {
  uint8_t winner = 0xFF;
  uint16_t best = 0xFFFF;
  for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
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
  for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
    if (players[i].joined && players[i].score > best) {
      best = players[i].score;
      winner = i;
    }
  }
  return winner;
}

uint8_t getRandomIndex(uint8_t *lastIdx, uint8_t max) {
  uint8_t idx;
  do { idx = random(max); } while (idx == *lastIdx);
  *lastIdx = idx;
  return idx;
}

// =============================================================================
// STATE HANDLERS
// =============================================================================
void handleIdleState() {
  if (stateStartTime == 0) {
    stateStartTime = millis();
    resetPlayers();
    currentRound = 0;
    neoMode = NEO_IDLE_RAINBOW;
    sendPacket(ID_BROADCAST, CMD_IDLE, 0, 0);
    sendPacket(ID_DISPLAY, DISP_IDLE, 0, 0);
    Serial.println(F("State: IDLE - Waiting for players"));
    
    // Audio: "Press to join"
    audio.queueSound(SND_PRESS_TO_JOIN);
  }
  
  // Check for display touch to skip
  if (Serial2.available() >= PACKET_SIZE && receivePacket()) {
    if (rxPacket[3] == TOUCH_SKIP_WAIT) {
      audio.queueSound(SND_BUTTON_CLICK);
      gameState = GAME_ASSIGN_IDS;
      stateStartTime = 0;
      currentAssignSlot = 0;
      return;
    }
  }
  
  // Timeout auto-start
  if (millis() - stateStartTime > TIMEOUT_JOIN) {
    gameState = GAME_ASSIGN_IDS;
    stateStartTime = 0;
    currentAssignSlot = 0;
  }
}

void handleAssignIDsState() {
  static unsigned long promptTime = 0;
  static bool waiting = false;
  
  if (stateStartTime == 0) {
    stateStartTime = millis();
    promptTime = 0;
    waiting = false;
    Serial.println(F("State: ASSIGN_IDS"));
  }
  
  // Check for touch skip (if 2+ players already joined)
  if (joinedCount >= 2 && receivePacket(0)) {
    if (rxPacket[3] == TOUCH_SKIP_WAIT) {
      Serial.println(F("Touch skip - starting with current players"));
      audio.queueSound(SND_BUTTON_CLICK);
      audio.queueSound(SND_GET_READY);
      gameState = GAME_COUNTDOWN;
      stateStartTime = 0;
      return;
    }
  }
  
  if (!waiting && currentAssignSlot < MAX_PLAYERS) {
    // Prompt next player
    Serial.printf("Prompting Player %d\n", currentAssignSlot + 1);
    sendPacket(ID_DISPLAY, DISP_PROMPT_JOIN, 0, currentAssignSlot + 1);
    sendPacket(ID_BROADCAST, CMD_ASSIGN_ID, 0, ID_STICK1 + currentAssignSlot);
    
    // Audio: "Player X"
    audio.playPlayerNumber(currentAssignSlot + 1);
    
    promptTime = millis();
    waiting = true;
  } else if (waiting) {
    // Check for response
    if (receivePacket(0)) {
      if (rxPacket[3] == CMD_OK && rxPacket[5] == ID_STICK1 + currentAssignSlot) {
        players[currentAssignSlot].joined = true;
        joinedCount++;
        Serial.printf("Player %d joined!\n", currentAssignSlot + 1);
        sendPacket(ID_DISPLAY, DISP_PLAYER_JOINED, 0, currentAssignSlot + 1);
        neoMode = NEO_STATUS;
        
        // Audio: "Ready"
        audio.queueSound(SND_READY);
        
        currentAssignSlot++;
        waiting = false;
      }
    }
    
    // Timeout for this slot (15s)
    if (millis() - promptTime > 15000) {
      Serial.printf("Player %d timeout, skipping\n", currentAssignSlot + 1);
      currentAssignSlot++;
      waiting = false;
    }
  }
  
  // All slots processed?
  if (currentAssignSlot >= MAX_PLAYERS) {
    if (joinedCount >= 2) {
      audio.queueSound(SND_GET_READY);
      gameState = GAME_COUNTDOWN;
    } else {
      Serial.println(F("Not enough players, returning to IDLE"));
      audio.queueSound(SND_ERROR_TONE);
      gameState = GAME_IDLE;
    }
    stateStartTime = 0;
  }
}

void handleCountdownState() {
  static uint8_t count = 3;
  static unsigned long lastTick = 0;
  
  if (stateStartTime == 0) {
    stateStartTime = millis();
    count = 3;
    lastTick = 0;
    resetRound();
    currentRound++;
    
    // Pick random mode
    if (random(2) == 0) {
      gameMode = MODE_REACTION;
      delayIndex = getRandomIndex(&lastDelayIdx, 3);
      sendPacket(ID_DISPLAY, DISP_REACTION_MODE, 0, 0);
      Serial.printf("Round %d: REACTION mode, delay=%dms\n", currentRound, REACT_DELAYS[delayIndex]);
      
      // Audio: "Reaction mode" + instruction
      audio.queueSound(SND_REACTION_MODE);
      audio.queueSound(SND_REACTION_INSTRUCT);
    } else {
      gameMode = MODE_SHAKE;
      targetIndex = getRandomIndex(&lastTargetIdx, 3);
      sendPacket(ID_DISPLAY, DISP_SHAKE_MODE, 0, SHAKE_TARGETS[targetIndex]);
      Serial.printf("Round %d: SHAKE mode, target=%d\n", currentRound, SHAKE_TARGETS[targetIndex]);
      
      // Audio: "Shake it" + target count
      audio.queueSound(SND_SHAKE_IT);
      audio.queueSound(SND_YOU_WILL_SHAKE);
      // Queue target number (10, 15, or 20)
      if (SHAKE_TARGETS[targetIndex] == 10) audio.queueSound(SND_NUM_10);
      else if (SHAKE_TARGETS[targetIndex] == 20) audio.queueSound(SND_NUM_20);
      else audio.queueSound(SND_NUM_15);
    }
    
    neoMode = NEO_COUNTDOWN_BLINK;
    delay(1000);
  }
  
  if (millis() - lastTick > 1000 && count > 0) {
    lastTick = millis();
    Serial.printf("Countdown: %d\n", count);
    sendPacket(ID_DISPLAY, DISP_COUNTDOWN, 0, count);
    sendPacket(ID_BROADCAST, CMD_COUNTDOWN, 0, count);
    
    // Audio: countdown number or tick
    audio.playCountdown(count);
    
    count--;
    
    if (count == 0) {
      delay(1000);
      // Send game start to all joysticks
      uint8_t param = (gameMode == MODE_SHAKE) ? SHAKE_TARGETS[targetIndex] : 0;
      sendPacket(ID_BROADCAST, CMD_GAME_START, gameMode, param);
      
      // Send GO via UART + hardware signal
      sendPacket(ID_BROADCAST, CMD_VIBRATE, 0, 0xFF);
      sendPacket(ID_DISPLAY, DISP_GO, 0, 0);
      pulseGO();  // Hardware timing sync!
      
      // Audio: Beep for GO!
      audio.queueSound(SND_BEEP);
      
      Serial.println(F("GO!"));
      
      gameState = (gameMode == MODE_SHAKE) ? GAME_SHAKE : GAME_REACTION;
      stateStartTime = 0;
    }
  }
}

void handleReactionState() {
  static bool signalSent = false;
  static unsigned long goTime = 0;
  
  if (stateStartTime == 0) {
    stateStartTime = millis();
    signalSent = false;
    goTime = 0;
    neoMode = NEO_RANDOM_FAST;
  }
  
  // Wait for random delay, then signal
  if (!signalSent && (millis() - stateStartTime) >= REACT_DELAYS[delayIndex]) {
    goTime = millis();
    signalSent = true;
    neoMode = NEO_FIXED_COLOR;
    neoColor = pixels.Color(0, 255, 0);
    
    // GO signal already sent in countdown, this is visual only
    Serial.println(F("SIGNAL!"));
  }
  
  // Timeout
  if (signalSent && (millis() - goTime) > TIMEOUT_REACTION) {
    gameState = GAME_COLLECT_RESULTS;
    stateStartTime = 0;
  }
}

void handleShakeState() {
  if (stateStartTime == 0) {
    stateStartTime = millis();
    neoMode = NEO_RANDOM_FAST;
  }
  
  if ((millis() - stateStartTime) > TIMEOUT_SHAKE) {
    gameState = GAME_COLLECT_RESULTS;
    stateStartTime = 0;
  }
}

void handleCollectResultsState() {
  static uint8_t currentPlayer = 0;
  static unsigned long tokenTime = 0;
  
  if (stateStartTime == 0) {
    stateStartTime = millis();
    currentPlayer = 0;
    tokenTime = 0;
    neoMode = NEO_STATUS;
    Serial.println(F("Collecting results..."));
  }
  
  if (currentPlayer < MAX_PLAYERS) {
    if (players[currentPlayer].joined && !players[currentPlayer].finished) {
      if (tokenTime == 0) {
        // Grant token
        sendPacket(ID_BROADCAST, CMD_TRANSMIT_TOKEN, 0, ID_STICK1 + currentPlayer);
        tokenTime = millis();
      } else if (millis() - tokenTime < TIMEOUT_TOKEN) {
        // Wait for response
        if (receivePacket(0)) {
          if (rxPacket[2] == ID_STICK1 + currentPlayer &&
              (rxPacket[3] == CMD_REACTION_DONE || rxPacket[3] == CMD_SHAKE_DONE)) {
            uint16_t time = ((uint16_t)rxPacket[4] << 8) | rxPacket[5];
            players[currentPlayer].reactionTime = time;
            players[currentPlayer].finished = true;
            Serial.printf("Player %d: %dms\n", currentPlayer + 1, time);
            currentPlayer++;
            tokenTime = 0;
          }
        }
      } else {
        // Timeout
        players[currentPlayer].finished = true;
        Serial.printf("Player %d: TIMEOUT\n", currentPlayer + 1);
        currentPlayer++;
        tokenTime = 0;
      }
    } else {
      currentPlayer++;
      tokenTime = 0;
    }
  } else {
    gameState = GAME_SHOW_RESULTS;
    stateStartTime = 0;
  }
}

void handleShowResultsState() {
  if (stateStartTime == 0) {
    stateStartTime = millis();
    
    uint8_t winner = findWinner();
    Serial.printf("Round winner: Player %d\n", winner + 1);
    
    // Send times to display (one command per player)
    // 0xFFFF = timeout (display shows red ring, no time)
    const uint8_t timeCommands[] = {DISP_TIME_P1, DISP_TIME_P2, DISP_TIME_P3, DISP_TIME_P4};
    for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
      if (players[i].joined) {
        uint16_t time = players[i].reactionTime;  // 0xFFFF if timeout
        sendPacket(ID_DISPLAY, timeCommands[i], (time >> 8) & 0xFF, time & 0xFF);
        delay(10);
      }
    }
    
    if (winner != 0xFF) {
      players[winner].score++;
      sendPacket(ID_DISPLAY, DISP_ROUND_WINNER, 0, winner + 1);
      
      // Audio: "Player X" + "Fastest"
      audio.playPlayerNumber(winner + 1);
      audio.queueSound(SND_FASTEST);
    } else {
      // No winner (all timeout)
      sendPacket(ID_DISPLAY, DISP_ROUND_WINNER, 0, 0);
    }
    
    // Send scores
    for (uint8_t i = 0; i < MAX_PLAYERS; i++) {
      if (players[i].joined) {
        sendPacket(ID_DISPLAY, DISP_SCORES, i + 1, players[i].score);
      }
    }
  }
  
  if (millis() - stateStartTime > 5000) {
    gameState = (currentRound >= TOTAL_ROUNDS) ? GAME_FINAL_WINNER : GAME_COUNTDOWN;
    stateStartTime = 0;
  }
}

void handleFinalWinnerState() {
  if (stateStartTime == 0) {
    stateStartTime = millis();
    uint8_t winner = findFinalWinner();
    Serial.printf("FINAL WINNER: Player %d\n", winner + 1);
    sendPacket(ID_DISPLAY, DISP_FINAL_WINNER, 0, winner + 1);
    neoMode = NEO_IDLE_RAINBOW;
    
    // Audio: Victory fanfare + "Player X Wins" + "Game Over"
    audio.queueSound(SND_VICTORY_FANFARE);
    audio.playPlayerWins(winner + 1);
    audio.queueSound(SND_GAME_OVER);
  }
  
  if (millis() - stateStartTime > 10000) {
    gameState = GAME_IDLE;
    stateStartTime = 0;
  }
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n=== Reaction Time Duel - Host ==="));
  
  // UART to all devices
  Serial2.begin(SERIAL_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);
  
  // Hardware control signals
  pinMode(PIN_GO, OUTPUT);
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_GO, LOW);
  digitalWrite(PIN_RST, LOW);
  
  // NeoPixel
  pixels.begin();
  pixels.setBrightness(NEOPIXEL_BRIGHTNESS);
  pixels.show();
  
  // Audio system (HSPI for SD to avoid CC1/CC2 conflict)
  if (audio.begin(0.5)) {
    Serial.println(F("Audio system ready"));
    audio.queueSound(SND_GET_READY);  // Startup sound
  } else {
    Serial.println(F("Audio init failed! Continuing without audio..."));
  }
  
  // Animation task on Core 1
  xTaskCreatePinnedToCore(animationTask, "Anim", 4096, NULL, 1, &animationTaskHandle, 1);
  
  // Random seed
  randomSeed(analogRead(36));
  
  // Initial state - start joining immediately on boot
  gameState = GAME_ASSIGN_IDS;
  stateStartTime = 0;
  currentAssignSlot = 0;
  resetPlayers();
  
  Serial.println(F("Host Ready!"));
  Serial.printf("UART: TX=%d, RX=%d\n", PIN_UART_TX, PIN_UART_RX);
  Serial.printf("Signals: GO=%d, RST=%d\n", PIN_GO, PIN_RST);
  Serial.printf("NeoPixel: %d (%d LEDs)\n", PIN_NEOPIXEL, NEOPIXEL_COUNT);
}
}

// =============================================================================
// MAIN LOOP
// =============================================================================
void loop() {
  // Non-blocking audio update - MUST call frequently
  audio.update();
  
  switch (gameState) {
    case GAME_IDLE:           handleIdleState(); break;
    case GAME_ASSIGN_IDS:     handleAssignIDsState(); break;
    case GAME_COUNTDOWN:      handleCountdownState(); break;
    case GAME_REACTION:       handleReactionState(); break;
    case GAME_SHAKE:          handleShakeState(); break;
    case GAME_COLLECT_RESULTS: handleCollectResultsState(); break;
    case GAME_SHOW_RESULTS:   handleShowResultsState(); break;
    case GAME_FINAL_WINNER:   handleFinalWinnerState(); break;
  }
  
  delay(1);
}
