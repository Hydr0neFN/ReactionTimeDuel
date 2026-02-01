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
// UI sounds
#define SND_BUTTON_CLICK      "/click.mp3"
#define SND_GET_READY         "/get_ready.mp3"
#define SND_PRESS_TO_JOIN     "/press_join.mp3"
#define SND_READY             "/ready.mp3"       // Player joined acknowledgment

// Game mode announcements
#define SND_REACTION_MODE     "/reaction.mp3"
#define SND_REACTION_INSTRUCT "/react_inst.mp3"
#define SND_SHAKE_IT          "/shake.mp3"
#define SND_YOU_WILL_SHAKE    "/will_shake.mp3"

// Numbers (for countdown and shake targets)
#define SND_NUM_1             "/one.mp3"
#define SND_NUM_2             "/two.mp3"
#define SND_NUM_3             "/three.mp3"
#define SND_NUM_10            "/ten.mp3"
#define SND_NUM_15            "/fifteen.mp3"
#define SND_NUM_20            "/twenty.mp3"
#define SND_BEEP              "/beep.mp3"

// Player announcements (combined "Player X" files)
#define SND_PLAYER_1          "/player1.mp3"    // "Player One"
#define SND_PLAYER_2          "/player2.mp3"    // "Player Two"
#define SND_PLAYER_3          "/player3.mp3"    // "Player Three"
#define SND_PLAYER_4          "/player4.mp3"    // "Player Four"

// Result phrases
#define SND_FASTEST           "/fastest.mp3"    // "Fastest"
#define SND_WINS              "/wins.mp3"       // "Wins"
#define SND_VICTORY_FANFARE   "/victory.mp3"
#define SND_GAME_OVER         "/gameover.mp3"
#define SND_ERROR_TONE        "/error.mp3"

// =============================================================================
// CONFIGURATION
// =============================================================================
#define AUDIO_QUEUE_SIZE      8
#define DEFAULT_VOLUME        4.0   // Max volume (range 0.0 - 4.0)

// I2S Pins (match schematic - GPIO25/26/27)
#define I2S_DOUT_PIN          25    // Data out to DAC
#define I2S_BCLK_PIN          26    // Bit clock
#define I2S_LRC_PIN           27    // Left/Right clock (Word Select)
// Use I2S port 1 to avoid conflicts with WiFi/ESP-NOW on port 0
#define I2S_PORT              1

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
    // Prevent double initialization
    if (out != nullptr) {
      Serial.println(F("Audio already initialized"));
      return true;
    }

    volume = vol;

    // Initialize SPIFFS
    if (!SPIFFS.begin(true)) {
      Serial.println(F("SPIFFS mount failed!"));
      return false;
    }

    // Use I2S port 1 to avoid conflicts with WiFi/ESP-NOW
    out = new AudioOutputI2S(I2S_PORT);
    if (!out) {
      Serial.println(F("Failed to create AudioOutputI2S!"));
      return false;
    }
    out->SetPinout(I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DOUT_PIN);
    out->SetGain(volume);

    mp3 = new AudioGeneratorMP3();
    if (!mp3) {
      Serial.println(F("Failed to create AudioGeneratorMP3!"));
      delete out;
      out = nullptr;
      return false;
    }

    Serial.printf("[AUDIO] Initialized on I2S port %d (DOUT=%d, BCLK=%d, LRC=%d)\n",
                  I2S_PORT, I2S_DOUT_PIN, I2S_BCLK_PIN, I2S_LRC_PIN);
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
  
  // Play a number (1-3 for countdown, 10/15/20 for shake)
  void playNumber(uint8_t num) {
    switch (num) {
      case 1:  queueSound(SND_NUM_1); break;
      case 2:  queueSound(SND_NUM_2); break;
      case 3:  queueSound(SND_NUM_3); break;
      case 10: queueSound(SND_NUM_10); break;
      case 15: queueSound(SND_NUM_15); break;
      case 20: queueSound(SND_NUM_20); break;
    }
  }

  // Play countdown number (3, 2, 1)
  void playCountdown(uint8_t num) {
    playNumber(num);
  }

  // Play "Player X" (combined file, e.g., "Player One")
  void playPlayerNumber(uint8_t player) {
    switch (player) {
      case 1: queueSound(SND_PLAYER_1); break;
      case 2: queueSound(SND_PLAYER_2); break;
      case 3: queueSound(SND_PLAYER_3); break;
      case 4: queueSound(SND_PLAYER_4); break;
    }
  }

  // Play "Player X" + "Wins" (e.g., "Player One" + "Wins")
  void playPlayerWins(uint8_t player) {
    playPlayerNumber(player);
    queueSound(SND_WINS);
  }

  // Play shake target (e.g., "Ten", "Fifteen", "Twenty")
  void playShakeTarget(uint8_t target) {
    playNumber(target);
  }
  
  // Must be called frequently (in loop)
  void update() {
    if (!out || !mp3) return;  // Not initialized

    // If currently playing, check if done
    if (isPlaying) {
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
        if (file) {
          delete file;
          file = nullptr;
        }
      }
    }

    // If not playing and queue has items, start next
    if (!isPlaying && queueHead != queueTail) {
      const char* filename = queue[queueHead];
      queueHead = (queueHead + 1) % AUDIO_QUEUE_SIZE;

      // Check if file exists
      if (SPIFFS.exists(filename)) {
        // Clean up any previous file object
        if (file) {
          delete file;
          file = nullptr;
        }

        file = new AudioFileSourceSPIFFS(filename);
        if (file && mp3->begin(file, out)) {
          isPlaying = true;
        } else {
          Serial.printf("[AUDIO] Failed to play: %s\n", filename);
          if (file) {
            delete file;
            file = nullptr;
          }
        }
      } else {
        Serial.printf("[AUDIO] File not found: %s\n", filename);
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