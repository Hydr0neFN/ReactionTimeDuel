/*
 * AudioManager.h - Non-blocking Audio Queue for ESP32
 * 
 * Uses SPIFFS for MP3 storage (no SD card needed)
 * Supports queuing multiple sounds for sequential playback
 * 
 * ACCESSIBILITY: Audio provides feedback for visually impaired players
 */

#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include "Arduino.h"
#include "SPIFFS.h"
#include "AudioFileSourceSPIFFS.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// =============================================================================
// SOUND FILE DEFINITIONS
// =============================================================================
// Files should be stored in SPIFFS at these paths
#define SND_BUTTON_CLICK      "/click.mp3"
#define SND_GET_READY         "/ready.mp3"
#define SND_PRESS_TO_JOIN     "/join.mp3"
#define SND_READY             "/joined.mp3"
#define SND_REACTION_MODE     "/reaction.mp3"
#define SND_REACTION_INSTRUCT "/react_i.mp3"
#define SND_SHAKE_IT          "/shake.mp3"
#define SND_YOU_WILL_SHAKE    "/willshk.mp3"
#define SND_NUM_10            "/10.mp3"
#define SND_NUM_15            "/15.mp3"
#define SND_NUM_20            "/20.mp3"
#define SND_BEEP              "/beep.mp3"
#define SND_COUNTDOWN_3       "/three.mp3"
#define SND_COUNTDOWN_2       "/two.mp3"
#define SND_COUNTDOWN_1       "/one.mp3"
#define SND_FASTEST           "/fastest.mp3"
#define SND_PLAYER_1          "/player1.mp3"
#define SND_PLAYER_2          "/player2.mp3"
#define SND_PLAYER_3          "/player3.mp3"
#define SND_PLAYER_4          "/player4.mp3"
#define SND_WINS              "/wins.mp3"
#define SND_VICTORY_FANFARE   "/victory.mp3"
#define SND_GAME_OVER         "/over.mp3"
#define SND_ERROR_TONE        "/error.mp3"

// =============================================================================
// CONFIGURATION
// =============================================================================
#define AUDIO_QUEUE_SIZE      8
#define DEFAULT_VOLUME        4.0   // Max volume (range 0.0 - 4.0)

// I2S Pins (match schematic)
#define I2S_DOUT_PIN          23
#define I2S_BCLK_PIN          26
#define I2S_LRC_PIN           25

// =============================================================================
// AUDIO MANAGER CLASS
// =============================================================================
class AudioManager {
public:
  AudioManager() : 
    mp3(nullptr), 
    file(nullptr), 
    out(nullptr),
    queueHead(0), 
    queueTail(0),
    isPlaying(false),
    volume(DEFAULT_VOLUME) {}
  
  ~AudioManager() {
    stop();
    if (mp3) delete mp3;
    if (file) delete file;
    if (out) delete out;
  }
  
  // Initialize audio system
  bool begin(float vol = DEFAULT_VOLUME) {
    volume = vol;
  
    // Initialize SPIFFS
    if (!SPIFFS.begin(true)) {
      Serial.println(F("SPIFFS mount failed!"));
      return false;
    }
  
    out = new AudioOutputI2S();
    out->SetPinout(I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DOUT_PIN);
    out->SetGain(volume);
  
    mp3 = new AudioGeneratorMP3();
  
    Serial.println(F("Audio system initialized (SPIFFS)"));
    return true;
  }
  
  // Queue a sound to play
  void queueSound(const char* filename) {
    uint8_t nextTail = (queueTail + 1) % AUDIO_QUEUE_SIZE;
    if (nextTail != queueHead) {  // Not full
      queue[queueTail] = filename;
      queueTail = nextTail;
    }
  }
  
  // Play countdown number
  void playCountdown(uint8_t num) {
    switch (num) {
      case 3: queueSound(SND_COUNTDOWN_3); break;
      case 2: queueSound(SND_COUNTDOWN_2); break;
      case 1: queueSound(SND_COUNTDOWN_1); break;
    }
  }
  
  // Play "Player X"
  void playPlayerNumber(uint8_t player) {
    switch (player) {
      case 1: queueSound(SND_PLAYER_1); break;
      case 2: queueSound(SND_PLAYER_2); break;
      case 3: queueSound(SND_PLAYER_3); break;
      case 4: queueSound(SND_PLAYER_4); break;
    }
  }
  
  // Play "Player X wins"
  void playPlayerWins(uint8_t player) {
    playPlayerNumber(player);
    queueSound(SND_WINS);
  }
  
  // Must be called frequently (in loop)
  void update() {
    // If currently playing, check if done
    if (isPlaying && mp3) {
      if (mp3->isRunning()) {
        if (!mp3->loop()) {
          mp3->stop();
          isPlaying = false;
          if (file) {
            delete file;
            file = nullptr;
          }
        }
      } else {
        isPlaying = false;
      }
    }
    
    // If not playing and queue has items, start next
    if (!isPlaying && queueHead != queueTail) {
      const char* filename = queue[queueHead];
      queueHead = (queueHead + 1) % AUDIO_QUEUE_SIZE;
      
      // Check if file exists
      if (SPIFFS.exists(filename)) {
        file = new AudioFileSourceSPIFFS(filename);
        if (mp3->begin(file, out)) {
          isPlaying = true;
        } else {
          delete file;
          file = nullptr;
        }
      }
    }
  }
  
  // Stop current playback
  void stop() {
    if (mp3 && mp3->isRunning()) {
      mp3->stop();
    }
    isPlaying = false;
    // Clear queue
    queueHead = queueTail = 0;
  }
  
  // Check if playing
  bool playing() const {
    return isPlaying;
  }
  
  // Set volume (0.0 - 4.0)
  void setVolume(float vol) {
    volume = vol;
    if (out) {
      out->SetGain(volume);
    }
  }

private:
  AudioGeneratorMP3 *mp3;
  AudioFileSourceSPIFFS *file;
  AudioOutputI2S *out;
  
  const char* queue[AUDIO_QUEUE_SIZE];
  uint8_t queueHead;
  uint8_t queueTail;
  
  bool isPlaying;
  float volume;
};

#endif // AUDIO_MANAGER_H