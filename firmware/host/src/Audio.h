#ifndef RTD_AUDIO_H
#define RTD_AUDIO_H

/*
 * Non-blocking MP3 queue (baseline AudioManager behaviour, DESIGN.md 8):
 * sounds come from SPIFFS, RtdSound values map to the file names of
 * firmware/host/data, the gain follows CFG_VOLUME = (v/10)^2.
 *
 * The decoder runs in its own FreeRTOS task on core 0; loop() (core 1)
 * only posts small commands through a queue and never waits for audio.
 */

#include <stdint.h>
#include <atomic>
#include "HostGame.h"

class AudioFileSourceSPIFFS;
class AudioGeneratorMP3;
class AudioOutputI2S;
struct AudioShared;

#define AUDIO_QUEUE_SIZE      8
#define AUDIO_SOUND_GAP_MS    250
#define AUDIO_TASK_STACK      8192
#define AUDIO_TASK_PRIORITY   2   /* below the Wi-Fi task */
#define AUDIO_TASK_CORE       0
#define AUDIO_TASK_IDLE_MS    20
#define AUDIO_TASK_TASK_NAME  "audio"

class Audio {
public:
  Audio();

  /* Mount SPIFFS, start I2S on port 1 (no conflict with Wi-Fi / ESP-NOW),
   * create the command queue and start the audio task. */
  bool begin();

  /* Queue a sound; SND_NONE is ignored. */
  void play(RtdSound s);

  /* Stop the current sound and clear the queue. */
  void stop();

  /* True when nothing plays and no PLAY is pending. */
  bool idle() const;

  /* Kept for the main loop; the decoder is advanced by the audio task. */
  void update();

  /* CFG_VOLUME 0..10 -> gain (v/10)^2, 10 = baseline loudness. */
  void setVolume(uint8_t volume);

private:
  static void taskMain(void* arg);
  void run();
  void handleStop();
  bool startNext();

  AudioShared* shared_;
  bool ready_;
  float gain_;
};

#endif /* RTD_AUDIO_H */
