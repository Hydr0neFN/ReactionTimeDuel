/*
 * AudioManager.h - Non-blocking Audio Queue System
 * Reaction Time Duel
 * 
 * Features:
 *   - ID-based track playback (1.mp3 to 28.mp3)
 *   - Non-blocking queue (plays in background)
 *   - Sequence chaining ("Player" + "1" + "Wins")
 *   - Thread-safe for dual-core ESP32
 * 
 * Pin Assignments (corrected for schematic):
 *   I2S DOUT: GPIO23 (DINS to MAX98357A)
 *   I2S BCLK: GPIO26
 *   I2S LRC:  GPIO25
 *   SD SCK:   GPIO14 (HSPI - avoids CC1/CC2 conflict!)
 *   SD MISO:  GPIO12 (HSPI)
 *   SD MOSI:  GPIO13 (HSPI)
 *   SD CS:    GPIO5
 */

#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include "AudioFileSourceSD.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// =============================================================================
// PIN DEFINITIONS (corrected from schematic)
// =============================================================================
// I2S pins (per schematic - MAX98357A connection)
#define AUDIO_I2S_DOUT    23    // DINS
#define AUDIO_I2S_BCLK    26    // BCLK
#define AUDIO_I2S_LRC     25    // LRC

// SD Card pins (HSPI to avoid GPIO18/19 conflict with CC1/CC2)
#define AUDIO_SD_CS       5
#define AUDIO_SD_SCK      14    // HSPI CLK
#define AUDIO_SD_MISO     12    // HSPI MISO  
#define AUDIO_SD_MOSI     13    // HSPI MOSI

// =============================================================================
// SOUND ID CATALOG (matches Protocol.h)
// =============================================================================
// Numbers (1-7)
#define SND_NUM_1              1
#define SND_NUM_2              2
#define SND_NUM_3              3
#define SND_NUM_4              4
#define SND_NUM_10             5
#define SND_NUM_15             6
#define SND_NUM_20             7

// Voice phrases (8-23)
#define SND_GET_READY          8
#define SND_321GO              9
#define SND_PLAYER            10
#define SND_READY             11
#define SND_DISCONNECTED      12
#define SND_SLOWEST           13
#define SND_FASTEST           14
#define SND_PRESS_TO_JOIN     15
#define SND_GAME_RULE         16
#define SND_REACTION_MODE     17
#define SND_REACTION_INSTRUCT 18
#define SND_SHAKE_IT          19
#define SND_YOU_WILL_SHAKE    20
#define SND_TIMES_WINS        21
#define SND_GAME_OVER         22
#define SND_WINS              23

// Sound effects (24-28)
#define SND_BEEP              24
#define SND_ERROR_TONE        25
#define SND_COUNTDOWN_TICK    26
#define SND_VICTORY_FANFARE   27
#define SND_BUTTON_CLICK      28

// =============================================================================
// QUEUE CONFIGURATION
// =============================================================================
#define AUDIO_QUEUE_SIZE      16
#define AUDIO_MAX_SEQUENCE    8

// =============================================================================
// AUDIO MANAGER CLASS
// =============================================================================
class AudioManager {
public:
  AudioManager();
  
  // Initialization
  bool begin(float volume = 0.5);
  
  // Queue management
  void queueSound(uint8_t soundId);
  void queueSequence(const uint8_t* soundIds, uint8_t count);
  void clearQueue();
  void stop();
  
  // Must be called regularly (from loop or task)
  void update();
  
  // Status
  bool isPlaying();
  bool isQueueEmpty();
  uint8_t getQueueCount();
  
  // Volume control (0.0 to 4.0)
  void setVolume(float vol);
  
  // Convenience methods
  void playPlayerNumber(uint8_t playerNum);  // "Player" + "1/2/3/4"
  void playPlayerWins(uint8_t playerNum);    // "Player" + "1/2/3/4" + "Wins"
  void playCountdown(uint8_t seconds);       // "3" or tick sound
  
private:
  AudioGeneratorMP3 *mp3;
  AudioFileSourceSD *file;
  AudioOutputI2S *out;
  SPIClass *hspi;
  
  // Circular queue
  uint8_t queue[AUDIO_QUEUE_SIZE];
  volatile uint8_t queueHead;
  volatile uint8_t queueTail;
  volatile uint8_t queueCount;
  
  // State
  bool initialized;
  bool playing;
  
  // Thread safety
  SemaphoreHandle_t mutex;
  
  // Internal methods
  bool playFile(uint8_t soundId);
  void buildFilename(uint8_t soundId, char* buffer);
  bool enqueue(uint8_t soundId);
  uint8_t dequeue();
};

// =============================================================================
// IMPLEMENTATION
// =============================================================================

AudioManager::AudioManager() {
  mp3 = nullptr;
  file = nullptr;
  out = nullptr;
  hspi = nullptr;
  queueHead = 0;
  queueTail = 0;
  queueCount = 0;
  initialized = false;
  playing = false;
  mutex = nullptr;
}

bool AudioManager::begin(float volume) {
  // Create mutex for thread safety
  mutex = xSemaphoreCreateMutex();
  if (!mutex) {
    Serial.println(F("[Audio] Mutex creation failed"));
    return false;
  }
  
  // Initialize HSPI for SD card (avoids VSPI conflict with CC1/CC2)
  hspi = new SPIClass(HSPI);
  hspi->begin(AUDIO_SD_SCK, AUDIO_SD_MISO, AUDIO_SD_MOSI, AUDIO_SD_CS);
  
  // Initialize SD card
  if (!SD.begin(AUDIO_SD_CS, *hspi)) {
    Serial.println(F("[Audio] SD card init failed!"));
    return false;
  }
  Serial.println(F("[Audio] SD card initialized"));
  
  // Initialize I2S output
  out = new AudioOutputI2S();
  out->SetPinout(AUDIO_I2S_BCLK, AUDIO_I2S_LRC, AUDIO_I2S_DOUT);
  out->SetGain(volume);
  
  // Create MP3 decoder
  mp3 = new AudioGeneratorMP3();
  
  initialized = true;
  Serial.println(F("[Audio] Manager ready"));
  return true;
}

void AudioManager::buildFilename(uint8_t soundId, char* buffer) {
  // Files named 1.mp3 to 28.mp3
  sprintf(buffer, "/%d.mp3", soundId);
}

bool AudioManager::playFile(uint8_t soundId) {
  if (!initialized) return false;
  
  // Stop any current playback
  if (mp3->isRunning()) {
    mp3->stop();
  }
  
  // Clean up previous file
  if (file) {
    delete file;
    file = nullptr;
  }
  
  // Build filename
  char filename[16];
  buildFilename(soundId, filename);
  
  // Check if file exists
  if (!SD.exists(filename)) {
    Serial.printf("[Audio] File not found: %s\n", filename);
    return false;
  }
  
  // Open file
  file = new AudioFileSourceSD(filename);
  if (!file) {
    Serial.printf("[Audio] Failed to open: %s\n", filename);
    return false;
  }
  
  // Start playback
  if (!mp3->begin(file, out)) {
    Serial.printf("[Audio] Failed to play: %s\n", filename);
    delete file;
    file = nullptr;
    return false;
  }
  
  playing = true;
  Serial.printf("[Audio] Playing: %s\n", filename);
  return true;
}

bool AudioManager::enqueue(uint8_t soundId) {
  if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return false;
  
  bool success = false;
  if (queueCount < AUDIO_QUEUE_SIZE) {
    queue[queueTail] = soundId;
    queueTail = (queueTail + 1) % AUDIO_QUEUE_SIZE;
    queueCount++;
    success = true;
  }
  
  xSemaphoreGive(mutex);
  return success;
}

uint8_t AudioManager::dequeue() {
  if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return 0;
  
  uint8_t soundId = 0;
  if (queueCount > 0) {
    soundId = queue[queueHead];
    queueHead = (queueHead + 1) % AUDIO_QUEUE_SIZE;
    queueCount--;
  }
  
  xSemaphoreGive(mutex);
  return soundId;
}

void AudioManager::queueSound(uint8_t soundId) {
  if (soundId == 0 || soundId > 28) return;
  enqueue(soundId);
}

void AudioManager::queueSequence(const uint8_t* soundIds, uint8_t count) {
  for (uint8_t i = 0; i < count && i < AUDIO_MAX_SEQUENCE; i++) {
    queueSound(soundIds[i]);
  }
}

void AudioManager::clearQueue() {
  if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return;
  queueHead = 0;
  queueTail = 0;
  queueCount = 0;
  xSemaphoreGive(mutex);
}

void AudioManager::stop() {
  clearQueue();
  if (mp3 && mp3->isRunning()) {
    mp3->stop();
  }
  playing = false;
}

void AudioManager::update() {
  if (!initialized) return;
  
  // If currently playing, keep the decoder running
  if (mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      playing = false;
    }
    return;
  }
  
  // Not playing - check queue for next sound
  playing = false;
  if (queueCount > 0) {
    uint8_t nextSound = dequeue();
    if (nextSound > 0) {
      playFile(nextSound);
    }
  }
}

bool AudioManager::isPlaying() {
  return playing || (mp3 && mp3->isRunning());
}

bool AudioManager::isQueueEmpty() {
  return queueCount == 0;
}

uint8_t AudioManager::getQueueCount() {
  return queueCount;
}

void AudioManager::setVolume(float vol) {
  if (out) {
    out->SetGain(constrain(vol, 0.0f, 4.0f));
  }
}

// =============================================================================
// CONVENIENCE METHODS
// =============================================================================

void AudioManager::playPlayerNumber(uint8_t playerNum) {
  // "Player" + "1/2/3/4"
  queueSound(SND_PLAYER);
  if (playerNum >= 1 && playerNum <= 4) {
    queueSound(playerNum);  // SND_NUM_1 to SND_NUM_4 = 1-4
  }
}

void AudioManager::playPlayerWins(uint8_t playerNum) {
  // "Player" + "1/2/3/4" + "Wins"
  queueSound(SND_PLAYER);
  if (playerNum >= 1 && playerNum <= 4) {
    queueSound(playerNum);
  }
  queueSound(SND_WINS);
}

void AudioManager::playCountdown(uint8_t seconds) {
  if (seconds >= 1 && seconds <= 4) {
    queueSound(seconds);  // 1, 2, 3, 4 map directly to file IDs
  } else {
    queueSound(SND_COUNTDOWN_TICK);
  }
}

#endif // AUDIO_MANAGER_H
