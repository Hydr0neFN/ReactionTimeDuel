#include "Audio.h"

#include <Arduino.h>
#include <SPIFFS.h>
#include <AudioFileSourceSPIFFS.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "Board.h"

/* I2S port 1 keeps port 0 free for Wi-Fi / ESP-NOW (baseline). */
#define AUDIO_I2S_PORT 1

/* Small fixed-size command: kind in cmd[0], payload in cmd[1]. */
#define AUDIO_CMD_LEN 2

/* Longer than the sound ring: STOP and GAIN still fit when the ring is full. */
#define AUDIO_CMD_QUEUE_LEN (AUDIO_QUEUE_SIZE * 2)

/* State shared between the loop task (core 1) and the audio task (core 0). */
struct AudioShared {
  QueueHandle_t cmds;                /* AUDIO_CMD_QUEUE_LEN commands of AUDIO_CMD_LEN */
  std::atomic<uint32_t> pending{0};  /* queued PLAYs + the one playing; 0 = idle */
  std::atomic<uint32_t> dropped{0};  /* PLAY requests refused because the queue was full */

  /* Owned by the audio task only; null until it has run once. */
  AudioGeneratorMP3* mp3;
  AudioFileSourceSPIFFS* file;
  AudioOutputI2S* out;
  float gain;
  bool playing;
  uint32_t lastEndMs;
  uint8_t queue[AUDIO_QUEUE_SIZE];
  uint8_t head;
  uint8_t tail;
};

Audio::Audio() : shared_(nullptr), ready_(false), gain_(1.0f) {}

static const char* audioFileName(RtdSound s) {
  switch (s) {
    case SND_BUTTON_CLICK:    return "/click.mp3";
    case SND_GET_READY:       return "/get_ready.mp3";
    case SND_PRESS_TO_JOIN:   return "/press_join.mp3";
    case SND_READY:           return "/ready.mp3";
    case SND_REACTION_MODE:   return "/reaction.mp3";
    case SND_REACTION_INSTRUCT: return "/react_inst.mp3";
    case SND_SHAKE_IT:        return "/shake.mp3";
    case SND_YOU_WILL_SHAKE:  return "/will_shake.mp3";
    case SND_NUM_1:           return "/one.mp3";
    case SND_NUM_2:           return "/two.mp3";
    case SND_NUM_3:           return "/three.mp3";
    case SND_NUM_10:          return "/ten.mp3";
    case SND_NUM_15:          return "/fifteen.mp3";
    case SND_NUM_20:          return "/twenty.mp3";
    case SND_BEEP:            return "/beep.mp3";
    case SND_PLAYER_1:        return "/player1.mp3";
    case SND_PLAYER_2:        return "/player2.mp3";
    case SND_PLAYER_3:        return "/player3.mp3";
    case SND_PLAYER_4:        return "/player4.mp3";
    case SND_FASTEST:         return "/fastest.mp3";
    case SND_WINS:            return "/wins.mp3";
    case SND_VICTORY_FANFARE: return "/victory.mp3";
    case SND_GAME_OVER:       return "/gameover.mp3";
    case SND_ERROR_TONE:      return "/error.mp3";
    default:                  return nullptr;
  }
}

bool Audio::begin() {
  if (!SPIFFS.begin(true)) {
    Serial.println("[AUDIO] SPIFFS mount failed");
    return false;
  }
  if (shared_ != nullptr) return true;

  shared_ = new AudioShared();
  if (shared_ == nullptr) return false;

  /* Amplifier GAIN grounded = maximum volume (baseline). */
  pinMode(PIN_AMP_GAIN, OUTPUT);
  digitalWrite(PIN_AMP_GAIN, LOW);

  shared_->cmds = xQueueCreate(AUDIO_CMD_QUEUE_LEN, AUDIO_CMD_LEN);
  if (shared_->cmds == nullptr) {
    delete shared_;
    shared_ = nullptr;
    return false;
  }

  /* The task does the I2S setup and creates the decoder objects itself. */
  if (xTaskCreatePinnedToCore(Audio::taskMain, AUDIO_TASK_TASK_NAME, AUDIO_TASK_STACK,
                              this, AUDIO_TASK_PRIORITY, nullptr,
                              AUDIO_TASK_CORE) != pdPASS) {
    vQueueDelete(shared_->cmds);
    delete shared_;
    shared_ = nullptr;
    return false;
  }

  ready_ = true;
  Serial.printf("[AUDIO] I2S port %d (DOUT=%d, BCLK=%d, LRC=%d)\n", AUDIO_I2S_PORT,
                PIN_I2S_DOUT, PIN_I2S_BCLK, PIN_I2S_LRC);
  return true;
}

void Audio::play(RtdSound s) {
  if (s == SND_NONE || shared_ == nullptr) return;
  /* Only this task increments, so the limit check cannot be overtaken. PLAY
   * commands never use more than AUDIO_QUEUE_SIZE queue places. */
  if (shared_->pending.load(std::memory_order_seq_cst) >= AUDIO_QUEUE_SIZE) {
    shared_->dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  /* Counted before the task can see the command, so idle() is false at once. */
  shared_->pending.fetch_add(1, std::memory_order_seq_cst);
  uint8_t cmd[AUDIO_CMD_LEN] = {'P', (uint8_t)s};
  if (xQueueSend(shared_->cmds, cmd, 0) != pdTRUE) {
    shared_->pending.fetch_sub(1, std::memory_order_seq_cst);
    shared_->dropped.fetch_add(1, std::memory_order_relaxed);
  }
}

void Audio::stop() {
  if (shared_ == nullptr) return;
  /* The command queue is longer than the sound ring, so STOP has a place.
   * Every count taken by play() is released exactly once: by the end of the
   * sound, by startNext() when the file cannot play, or by handleStop() for
   * the playing sound and each ring entry. All three run in the audio task,
   * in command order, so the counter always returns to 0 and idle() turns
   * true only after the task has stopped the decoder. */
  uint8_t cmd[AUDIO_CMD_LEN] = {'S', 0};
  xQueueSend(shared_->cmds, cmd, 0);
}

bool Audio::idle() const {
  return shared_ == nullptr || shared_->pending.load(std::memory_order_seq_cst) == 0;
}

void Audio::setVolume(uint8_t volume) {
  if (volume > 10) volume = 10;
  float v = (float)volume / 10.0f;
  gain_ = v * v;
  if (shared_ == nullptr) return;
  uint8_t cmd[AUDIO_CMD_LEN] = {'G', volume};
  xQueueSend(shared_->cmds, cmd, 0);
}

void Audio::update() {}

void Audio::taskMain(void* arg) {
  static_cast<Audio*>(arg)->run();
  vTaskDelete(nullptr);
}

void Audio::run() {
  AudioShared& sh = *shared_;

  sh.gain = gain_;
  sh.out = new AudioOutputI2S(AUDIO_I2S_PORT);
  if (sh.out != nullptr) {
    sh.out->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT);
    sh.out->SetGain(sh.gain);
  }
  sh.mp3 = new AudioGeneratorMP3();
  if (sh.mp3 == nullptr || sh.out == nullptr) {
    Serial.println("[AUDIO] out of memory");
  }

  for (;;) {
    /* While a sound plays, one pass decodes, drains requests, then yields. */
    bool decoding = false;
    if (sh.playing) {
      if (sh.mp3->isRunning()) {
        decoding = sh.mp3->loop();
      }
      if (!decoding) {
        /* Ended (or already not running): close it like the baseline did. */
        if (sh.mp3->isRunning()) sh.mp3->stop();
        sh.playing = false;
        sh.lastEndMs = millis();
        if (sh.file != nullptr) {
          delete sh.file;
          sh.file = nullptr;
        }
        /* Keep reporting busy while another PLAY is still pending (the gap). */
        if (sh.pending.fetch_sub(1, std::memory_order_seq_cst) > 1 ||
            sh.head != sh.tail) {
          decoding = true;
        }
      }
    }

    /* Drain all pending commands; PLAY keeps the pending counter, STOP
     * (in handleStop) drops the pending count of each queued PLAY. */
    uint8_t cmd[AUDIO_CMD_LEN];
    while (xQueueReceive(sh.cmds, cmd, 0) == pdTRUE) {
      if (cmd[0] == 'S') {
        handleStop();
      } else if (cmd[0] == 'G') {
        float v = (float)cmd[1] / 10.0f;
        sh.gain = v * v;
        if (sh.out != nullptr) sh.out->SetGain(sh.gain);
      } else {
        uint8_t next = (uint8_t)((sh.tail + 1) % AUDIO_QUEUE_SIZE);
        if (next == sh.head) {
          /* Cannot happen: the command queue is not larger than the ring. */
          sh.pending.fetch_sub(1, std::memory_order_seq_cst);
        } else {
          sh.queue[sh.tail] = (uint8_t)cmd[1];
          sh.tail = next;
        }
      }
    }

    /* The gap only delays the start, not the decode. */
    if (!sh.playing) startNext();

    if (decoding || sh.playing) {
      vTaskDelay(1);  /* let the other core 0 tasks run */
      continue;
    }

    /* Nothing plays: wait for a command or the end of the sound gap. Peek
     * only: the command stays in the queue for the drain loop above. */
    xQueuePeek(sh.cmds, cmd, pdMS_TO_TICKS(AUDIO_TASK_IDLE_MS));
  }
}

void Audio::handleStop() {
  AudioShared& sh = *shared_;
  if (sh.playing) {
    if (sh.mp3 != nullptr && sh.mp3->isRunning()) sh.mp3->stop();
    sh.playing = false;
    sh.lastEndMs = 0;
    if (sh.file != nullptr) {
      delete sh.file;
      sh.file = nullptr;
    }
    sh.pending.fetch_sub(1, std::memory_order_seq_cst);
  }

  /* Each queued PLAY holds one pending count (play() adds it, startNext()
   * removes it), so dropping the entries drops exactly those counts. */
  while (sh.head != sh.tail) {
    sh.head = (uint8_t)((sh.head + 1) % AUDIO_QUEUE_SIZE);
    sh.pending.fetch_sub(1, std::memory_order_seq_cst);
  }
  sh.head = sh.tail = 0;
}

bool Audio::startNext() {
  AudioShared& sh = *shared_;
  if (sh.playing || sh.head == sh.tail) return false;
  if (sh.lastEndMs != 0 && (millis() - sh.lastEndMs) < AUDIO_SOUND_GAP_MS) return false;

  RtdSound s = (RtdSound)sh.queue[sh.head];
  sh.head = (uint8_t)((sh.head + 1) % AUDIO_QUEUE_SIZE);

  const char* name = audioFileName(s);
  bool started = false;
  if (sh.mp3 == nullptr || sh.out == nullptr) {
    /* Out of memory at startup: drop the sound like a missing file. */
  } else if (name == nullptr) {
    /* SND_NONE is never queued; unknown sounds are skipped. */
  } else if (!SPIFFS.exists(name)) {
    Serial.printf("[AUDIO] Missing file %s\n", name);
  } else {
    if (sh.file != nullptr) {
      delete sh.file;
      sh.file = nullptr;
    }
    sh.file = new AudioFileSourceSPIFFS(name);
    if (sh.file != nullptr && sh.mp3->begin(sh.file, sh.out)) {
      sh.playing = true;
      started = true;
    } else {
      Serial.printf("[AUDIO] Failed to play %s\n", name);
      if (sh.file != nullptr) {
        delete sh.file;
        sh.file = nullptr;
      }
    }
  }

  /* The sound ended right away (missing or refused): count it as done. */
  if (!started) sh.pending.fetch_sub(1, std::memory_order_seq_cst);
  return started;
}
