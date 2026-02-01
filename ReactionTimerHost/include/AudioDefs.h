/*
 * AudioDefs.h - Audio File Definitions
 * ESP32-S3 Master only
 */

#ifndef AUDIODEFS_H
#define AUDIODEFS_H

// =============================================================================
// SOUND IDS
// =============================================================================
#define SND_NUM_1         1
#define SND_NUM_2         2
#define SND_NUM_3         3
#define SND_NUM_4         4
#define SND_NUM_10        5
#define SND_NUM_15        6
#define SND_NUM_20        7
#define SND_GET_READY     8
#define SND_PLAYER        10
#define SND_READY         11
#define SND_FASTEST       14
#define SND_PRESS_TO_JOIN 15
#define SND_REACTION_MODE 17
#define SND_SHAKE_IT      19
#define SND_GAME_OVER     22
#define SND_WINS          23
#define SND_BEEP          24
#define SND_ERROR         25
#define SND_VICTORY       27

#define NUM_SOUNDS        29
#define AUDIO_QUEUE_SIZE  8

// =============================================================================
// FILE PATHS (SPIFFS)
// =============================================================================
static const char* const SOUND_FILES[NUM_SOUNDS] = {
  "",             // 0 unused
  "/1.mp3",       // 1
  "/2.mp3",       // 2
  "/3.mp3",       // 3
  "/4.mp3",       // 4
  "/10.mp3",      // 5
  "/15.mp3",      // 6
  "/20.mp3",      // 7
  "/ready.mp3",   // 8
  "/321go.mp3",   // 9
  "/player.mp3",  // 10
  "/joined.mp3",  // 11
  "/disc.mp3",    // 12
  "/slowest.mp3", // 13
  "/fastest.mp3", // 14
  "/join.mp3",    // 15
  "/rule.mp3",    // 16
  "/reaction.mp3",// 17
  "/react_i.mp3", // 18
  "/shake.mp3",   // 19
  "/willshk.mp3", // 20
  "/times.mp3",   // 21
  "/over.mp3",    // 22
  "/wins.mp3",    // 23
  "/beep.mp3",    // 24
  "/error.mp3",   // 25
  "/tick.mp3",    // 26
  "/victory.mp3", // 27
  "/click.mp3",   // 28
};

#endif // AUDIODEFS_H
