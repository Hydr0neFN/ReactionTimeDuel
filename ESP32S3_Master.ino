/*
 * ESP32S3_Master.ino - Reaction Time Duel - Unified Controller
 * 
 * Hardware: Waveshare ESP32-S3-Touch-LCD-7 (800x480)
 * Functions: Game logic + Display + NeoPixel + Audio + ESP-NOW
 * 
 * Core 0: Game logic, ESP-NOW, Audio
 * Core 1: LVGL rendering, NeoPixel animation
 */

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>
#include "SPIFFS.h"
#include "AudioFileSourceSPIFFS.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

#include "lvgl.h"
#include "display.h"
#include "ui.h"

#include "Protocol.h"
#include "GameTypes.h"
#include "AudioDefs.h"

// =============================================================================
// PINS - Verify against your wiring!
// =============================================================================
#define PIN_NEOPIXEL      8
#define PIN_I2S_BCLK      15
#define PIN_I2S_LRC       16
#define PIN_I2S_DOUT      6

// =============================================================================
// MAC ADDRESSES - UPDATE THESE!
// =============================================================================
uint8_t mac_stick1[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01};  // TODO
uint8_t mac_stick2[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x02};  // TODO
uint8_t mac_stick3[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x03};  // TODO
uint8_t mac_stick4[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x04};  // TODO
uint8_t mac_broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// =============================================================================
// GLOBALS
// =============================================================================
Adafruit_NeoPixel pixels(NEOPIXEL_COUNT, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

AudioGeneratorMP3* mp3 = nullptr;
AudioFileSourceSPIFFS* audioFile = nullptr;
AudioOutputI2S* audioOut = nullptr;

volatile NeoMode neoMode = NEO_IDLE_RAINBOW;
volatile uint32_t fixedColor = COLOR_GREEN;
volatile uint32_t ringColors[NUM_RINGS] = {0};
volatile bool ringOverride[NUM_RINGS] = {false};

volatile uint8_t audioQueue[AUDIO_QUEUE_SIZE];
volatile uint8_t audioQHead = 0, audioQTail = 0;
volatile bool audioPlaying = false;
portMUX_TYPE audioMux = portMUX_INITIALIZER_UNLOCKED;

GameState gameState = GAME_IDLE;
Player players[MAX_PLAYERS];
uint8_t joinedCount = 0;
uint8_t currentRound = 0;
uint8_t gameMode = 0;
uint8_t delayIndex = 0, targetIndex = 0;
uint8_t lastDelayIdx = 0xFF, lastTargetIdx = 0xFF;

unsigned long stateEnterTime = 0;
unsigned long lastEventTime = 0;
uint8_t countdownNum = 3;

#define RX_QUEUE_SIZE 16
GamePacket rxQueue[RX_QUEUE_SIZE];
volatile uint8_t rxHead = 0, rxTail = 0;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

volatile uint8_t lastRecvMAC[6];
volatile bool macUpdated = false;

SemaphoreHandle_t lvglMutex;

uint8_t rainbowOffset = 0;
bool blinkState = false;
unsigned long lastAnimTime = 0, lastBlinkTime = 0;

// =============================================================================
// PACKET FUNCTIONS
// =============================================================================
void sendPacket(uint8_t* mac, uint8_t dest, uint8_t cmd, uint16_t data) {
  GamePacket pkt;
  buildPacket(&pkt, dest, ID_HOST, cmd, data);
  esp_now_send(mac, (uint8_t*)&pkt, sizeof(pkt));
}

void broadcast(uint8_t cmd, uint16_t data) {
  sendPacket(mac_broadcast, ID_BROADCAST, cmd, data);
}

// =============================================================================
// AUDIO
// =============================================================================
void queueSound(uint8_t id) {
  if (id == 0 || id >= NUM_SOUNDS) return;
  portENTER_CRITICAL(&audioMux);
  uint8_t next = (audioQTail + 1) % AUDIO_QUEUE_SIZE;
  if (next != audioQHead) { audioQueue[audioQTail] = id; audioQTail = next; }
  portEXIT_CRITICAL(&audioMux);
}

void updateAudio() {
  if (audioPlaying && mp3) {
    if (mp3->isRunning()) {
      if (!mp3->loop()) {
        mp3->stop(); audioPlaying = false;
        if (audioFile) { delete audioFile; audioFile = nullptr; }
      }
    } else audioPlaying = false;
  }
  
  if (!audioPlaying) {
    uint8_t soundId = 0;
    portENTER_CRITICAL(&audioMux);
    if (audioQHead != audioQTail) {
      soundId = audioQueue[audioQHead];
      audioQHead = (audioQHead + 1) % AUDIO_QUEUE_SIZE;
    }
    portEXIT_CRITICAL(&audioMux);
    
    if (soundId > 0 && soundId < NUM_SOUNDS && SPIFFS.exists(SOUND_FILES[soundId])) {
      audioFile = new AudioFileSourceSPIFFS(SOUND_FILES[soundId]);
      if (mp3->begin(audioFile, audioOut)) audioPlaying = true;
      else { if (audioFile) { delete audioFile; audioFile = nullptr; } }
    }
  }
}

// =============================================================================
// NEOPIXEL
// =============================================================================
uint32_t wheel(uint8_t pos) {
  pos = 255 - pos;
  if (pos < 85) return pixels.Color(255 - pos * 3, 0, pos * 3);
  if (pos < 170) { pos -= 85; return pixels.Color(0, pos * 3, 255 - pos * 3); }
  pos -= 170; return pixels.Color(pos * 3, 255 - pos * 3, 0);
}

void setRing(uint8_t ring, uint32_t color) {
  if (ring >= NUM_RINGS) return;
  uint8_t start = ring * LEDS_PER_RING;
  for (uint8_t i = 0; i < LEDS_PER_RING; i++) pixels.setPixelColor(start + i, color);
}

void setAllRings(uint32_t color) {
  for (int i = 0; i < NEOPIXEL_COUNT; i++) pixels.setPixelColor(i, color);
}

void setNeoMode(NeoMode mode) {
  neoMode = mode;
  for (int i = 0; i < NUM_RINGS; i++) ringOverride[i] = false;
}

void setRingColor(uint8_t ring, uint32_t color) {
  if (ring < NUM_RINGS) { ringOverride[ring] = true; ringColors[ring] = color; }
}

void updateNeoPixel() {
  unsigned long now = millis();
  switch (neoMode) {
    case NEO_OFF: setAllRings(0); pixels.show(); break;
    case NEO_IDLE_RAINBOW:
      if (now - lastAnimTime >= 50) {
        lastAnimTime = now;
        for (int i = 0; i < NEOPIXEL_COUNT; i++)
          pixels.setPixelColor(i, wheel((i * 256 / NEOPIXEL_COUNT + rainbowOffset) & 255));
        pixels.show(); rainbowOffset++;
      }
      break;
    case NEO_STATUS:
      for (uint8_t r = 0; r < NUM_RINGS; r++) {
        if (r == 2) setRing(r, wheel(rainbowOffset));
        else if (ringOverride[r]) setRing(r, ringColors[r]);
        else setRing(r, COLOR_YELLOW);
      }
      pixels.show();
      if (now - lastAnimTime >= 50) { lastAnimTime = now; rainbowOffset++; }
      break;
    case NEO_RANDOM_FAST:
      if (now - lastAnimTime >= 100) {
        lastAnimTime = now;
        for (int i = 0; i < NEOPIXEL_COUNT; i++) pixels.setPixelColor(i, wheel(random(256)));
        pixels.show();
      }
      break;
    case NEO_FIXED_COLOR: setAllRings(fixedColor); pixels.show(); break;
    case NEO_COUNTDOWN:
      if (now - lastBlinkTime >= 250) {
        lastBlinkTime = now; blinkState = !blinkState;
        setAllRings(blinkState ? COLOR_RED : 0); pixels.show();
      }
      break;
  }
}

// =============================================================================
// ESP-NOW
// =============================================================================
void OnDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(GamePacket)) return;
  GamePacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (!validatePacket(&pkt) || pkt.src_id == ID_HOST) return;
  
  memcpy((void*)lastRecvMAC, info->src_addr, 6);
  macUpdated = true;
  
  portENTER_CRITICAL(&rxMux);
  uint8_t next = (rxTail + 1) % RX_QUEUE_SIZE;
  if (next != rxHead) { memcpy(&rxQueue[rxTail], &pkt, sizeof(pkt)); rxTail = next; }
  portEXIT_CRITICAL(&rxMux);
}

bool getPacket(GamePacket* pkt) {
  portENTER_CRITICAL(&rxMux);
  if (rxHead == rxTail) { portEXIT_CRITICAL(&rxMux); return false; }
  memcpy(pkt, &rxQueue[rxHead], sizeof(GamePacket));
  rxHead = (rxHead + 1) % RX_QUEUE_SIZE;
  portEXIT_CRITICAL(&rxMux);
  return true;
}

// =============================================================================
// GAME HELPERS
// =============================================================================
void resetPlayers() {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    players[i] = {false, false, TIME_PENALTY, 0, {0}};
  }
  joinedCount = 0;
}

void resetRound() {
  for (int i = 0; i < MAX_PLAYERS; i++) {
    players[i].finished = false;
    players[i].reactionTime = TIME_PENALTY;
  }
  for (int i = 0; i < NUM_RINGS; i++) ringOverride[i] = false;
}

uint8_t findWinner() {
  uint8_t winner = NO_WINNER;
  uint16_t best = TIME_PENALTY;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (players[i].joined && players[i].finished && players[i].reactionTime < best) {
      best = players[i].reactionTime; winner = i;
    }
  }
  return winner;
}

uint8_t findFinalWinner() {
  uint8_t winner = NO_WINNER;
  uint8_t best = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (players[i].joined && players[i].score > best) {
      best = players[i].score; winner = i;
    }
  }
  return winner;
}

void enterState(GameState s) {
  gameState = s;
  stateEnterTime = millis();
  lastEventTime = millis();
}

// =============================================================================
// UI (Thread-safe wrapper)
// =============================================================================
void ui_update(void (*fn)()) {
  if (xSemaphoreTake(lvglMutex, pdMS_TO_TICKS(10))) { fn(); xSemaphoreGive(lvglMutex); }
}

void ui_showIdle_impl() {
  lv_obj_clear_flag(ui_centerCircle, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui_imgStart, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui_imgGo, LV_OBJ_FLAG_HIDDEN);
}

void ui_showGo_impl() {
  lv_obj_add_flag(ui_imgStart, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(ui_imgGo, LV_OBJ_FLAG_HIDDEN);
}

// =============================================================================
// GAME STATE MACHINE
// =============================================================================
void updateGame() {
  unsigned long now = millis();
  unsigned long elapsed = now - stateEnterTime;
  unsigned long sinceEvent = now - lastEventTime;
  
  GamePacket pkt;
  while (getPacket(&pkt)) {
    Serial.printf("[RX] src=%d cmd=0x%02X\n", pkt.src_id, pkt.cmd);
    
    if (pkt.cmd == CMD_REQ_ID && gameState == GAME_JOINING && joinedCount < MAX_PLAYERS) {
      uint8_t slot = 0;
      while (slot < MAX_PLAYERS && players[slot].joined) slot++;
      if (slot < MAX_PLAYERS && macUpdated) {
        memcpy(players[slot].mac, (void*)lastRecvMAC, 6);
        macUpdated = false;
        sendPacket(players[slot].mac, ID_STICK1 + slot, CMD_OK, ID_STICK1 + slot);
        players[slot].joined = true;
        joinedCount++;
        setRingColor(playerToRing(slot), COLOR_GREEN);
        queueSound(SND_PLAYER); queueSound(slot + 1); queueSound(SND_READY);
        Serial.printf("P%d joined\n", slot + 1);
        lastEventTime = now;
      }
    }
    
    if (pkt.cmd == CMD_REACTION_DONE || pkt.cmd == CMD_SHAKE_DONE) {
      uint8_t pid = pkt.src_id - ID_STICK1;
      if (pid < MAX_PLAYERS && players[pid].joined && !players[pid].finished) {
        players[pid].reactionTime = packetData(&pkt);
        players[pid].finished = true;
        if (players[pid].reactionTime == TIME_PENALTY) {
          setRingColor(playerToRing(pid), COLOR_RED);
          queueSound(SND_ERROR);
        }
        Serial.printf("P%d: %dms\n", pid + 1, players[pid].reactionTime);
      }
    }
  }
  
  switch (gameState) {
    case GAME_IDLE:
      if (elapsed == 0) {
        resetPlayers(); currentRound = 0;
        setNeoMode(NEO_IDLE_RAINBOW);
        ui_update(ui_showIdle_impl);
        Serial.println(F("IDLE"));
      }
      if (elapsed > DURATION_IDLE) enterState(GAME_JOINING);
      break;
      
    case GAME_JOINING:
      if (elapsed == 0) {
        setNeoMode(NEO_STATUS);
        queueSound(SND_PRESS_TO_JOIN);
        Serial.println(F("JOINING"));
      }
      if ((joinedCount >= 2 && sinceEvent > JOIN_IDLE_TIME) || elapsed > TIMEOUT_JOIN) {
        if (joinedCount >= 2) { queueSound(SND_GET_READY); enterState(GAME_COUNTDOWN); }
        else { queueSound(SND_ERROR); enterState(GAME_IDLE); }
      }
      break;
      
    case GAME_COUNTDOWN:
      if (elapsed == 0) {
        countdownNum = 3; resetRound(); currentRound++;
        if (random(2) == 0) {
          gameMode = MODE_REACTION;
          do { delayIndex = random(NUM_REACT_DELAYS); } while (delayIndex == lastDelayIdx);
          lastDelayIdx = delayIndex;
          queueSound(SND_REACTION_MODE);
        } else {
          gameMode = MODE_SHAKE;
          do { targetIndex = random(NUM_SHAKE_TARGETS); } while (targetIndex == lastTargetIdx);
          lastTargetIdx = targetIndex;
          queueSound(SND_SHAKE_IT);
          uint8_t t = SHAKE_TARGETS[targetIndex];
          queueSound(t == 10 ? SND_NUM_10 : (t == 15 ? SND_NUM_15 : SND_NUM_20));
        }
        setNeoMode(NEO_COUNTDOWN);
        Serial.printf("Round %d: %s\n", currentRound, gameMode == MODE_REACTION ? "REACT" : "SHAKE");
      }
      if (sinceEvent >= 1000 && countdownNum > 0) {
        lastEventTime = now;
        broadcast(CMD_COUNTDOWN, countdownNum);
        queueSound(countdownNum);
        countdownNum--;
      }
      if (elapsed > DURATION_COUNTDOWN) {
        uint16_t param = (gameMode == MODE_SHAKE) ? SHAKE_TARGETS[targetIndex] : 0;
        broadcast(CMD_GAME_START, (gameMode << 8) | param);
        if (gameMode == MODE_SHAKE) {
          broadcast(CMD_VIBRATE, VIBRATE_GO);
          setNeoMode(NEO_RANDOM_FAST);
          ui_update(ui_showGo_impl);
          queueSound(SND_BEEP);
          enterState(GAME_SHAKE_ACTIVE);
        } else {
          enterState(GAME_REACTION_WAIT);
        }
      }
      break;
      
    case GAME_REACTION_WAIT:
      if (elapsed == 0) {
        ui_update(ui_showGo_impl);
        setNeoMode(NEO_RANDOM_FAST);
      }
      if (elapsed >= REACT_DELAYS[delayIndex]) {
        broadcast(CMD_VIBRATE, VIBRATE_GO);
        queueSound(SND_BEEP);
        neoMode = NEO_FIXED_COLOR;
        fixedColor = COLOR_GREEN;
        enterState(GAME_REACTION_ACTIVE);
      }
      break;
      
    case GAME_REACTION_ACTIVE:
    case GAME_SHAKE_ACTIVE: {
      bool allDone = true;
      for (int i = 0; i < MAX_PLAYERS; i++)
        if (players[i].joined && !players[i].finished) allDone = false;
      uint32_t timeout = (gameState == GAME_REACTION_ACTIVE) ? TIMEOUT_REACTION : TIMEOUT_SHAKE;
      if (allDone || elapsed > timeout) enterState(GAME_RESULTS);
    } break;
      
    case GAME_RESULTS:
      if (elapsed == 0) {
        for (int i = 0; i < MAX_PLAYERS; i++) {
          if (players[i].joined && !players[i].finished) {
            players[i].finished = true;
            players[i].reactionTime = TIME_PENALTY;
          }
        }
        uint8_t winner = findWinner();
        setNeoMode(NEO_STATUS);
        for (int i = 0; i < MAX_PLAYERS; i++) {
          if (players[i].joined) {
            uint8_t ring = playerToRing(i);
            if (players[i].reactionTime == TIME_PENALTY) setRingColor(ring, COLOR_RED);
            else if (i == winner) setRingColor(ring, COLOR_GREEN);
            else setRingColor(ring, COLOR_YELLOW);
          }
        }
        if (winner != NO_WINNER) {
          players[winner].score++;
          queueSound(SND_PLAYER); queueSound(winner + 1); queueSound(SND_FASTEST);
        }
        broadcast(CMD_IDLE, 0);
      }
      if (elapsed > DURATION_RESULTS) {
        enterState(currentRound >= TOTAL_ROUNDS ? GAME_FINAL : GAME_COUNTDOWN);
      }
      break;
      
    case GAME_FINAL:
      if (elapsed == 0) {
        uint8_t winner = findFinalWinner();
        setNeoMode(NEO_IDLE_RAINBOW);
        queueSound(SND_VICTORY);
        queueSound(SND_PLAYER); queueSound(winner + 1);
        queueSound(SND_WINS); queueSound(SND_GAME_OVER);
        Serial.printf("WINNER: P%d\n", winner + 1);
      }
      if (elapsed > DURATION_FINAL) enterState(GAME_IDLE);
      break;
  }
}

// =============================================================================
// CORE 1 TASK (LVGL + NeoPixel)
// =============================================================================
void core1Task(void* param) {
  TickType_t lastWake = xTaskGetTickCount();
  while (true) {
    if (xSemaphoreTake(lvglMutex, pdMS_TO_TICKS(10))) {
      lv_tick_inc(5);
      lv_timer_handler();
      xSemaphoreGive(lvglMutex);
    }
    updateNeoPixel();
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(5));
  }
}

// =============================================================================
// ESP-NOW SETUP
// =============================================================================
void setupESPNOW() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  
  if (esp_now_init() != ESP_OK) { Serial.println(F("ESP-NOW fail")); return; }
  esp_now_register_recv_cb(OnDataRecv);
  
  esp_now_peer_info_t peer = {};
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  
  memcpy(peer.peer_addr, mac_broadcast, 6); esp_now_add_peer(&peer);
  
  uint8_t* const macs[] = {mac_stick1, mac_stick2, mac_stick3, mac_stick4};
  for (int i = 0; i < 4; i++) {
    memcpy(peer.peer_addr, macs[i], 6);
    esp_now_add_peer(&peer);
  }
  Serial.print(F("MAC: ")); Serial.println(WiFi.macAddress());
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n=== Reaction Reimagined ==="));
  
  lvglMutex = xSemaphoreCreateMutex();
  
  pixels.begin();
  pixels.setBrightness(NEO_BRIGHTNESS);
  pixels.show();
  
  if (!SPIFFS.begin(true)) Serial.println(F("SPIFFS fail"));
  
  audioOut = new AudioOutputI2S();
  audioOut->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
  audioOut->SetGain(4.0);
  mp3 = new AudioGeneratorMP3();
  
  lv_init();
  display_init();
  ui_init();
  
  setupESPNOW();
  randomSeed(esp_random());
  
  xTaskCreatePinnedToCore(core1Task, "Core1", 16384, NULL, 1, NULL, 1);
  
  enterState(GAME_IDLE);
  Serial.println(F("Ready"));
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
  updateGame();
  updateAudio();
}
