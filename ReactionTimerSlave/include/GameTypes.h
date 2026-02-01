/*
 * GameTypes.h - Game Constants and Types
 * ESP32-S3 Master only
 */

#ifndef GAMETYPES_H
#define GAMETYPES_H

#include <stdint.h>

// =============================================================================
// GAME CONSTANTS
// =============================================================================
#define MAX_PLAYERS       4
#define TOTAL_ROUNDS      5

// =============================================================================
// TIMING (milliseconds)
// =============================================================================
#define TIMEOUT_JOIN      30000   // Max join phase
#define TIMEOUT_REACTION  10000   // Max time after GO
#define TIMEOUT_SHAKE     30000   // Max shake phase
#define JOIN_IDLE_TIME    5000    // Auto-start after last join

#define DURATION_IDLE     3000
#define DURATION_COUNTDOWN 4000
#define DURATION_RESULTS  5000
#define DURATION_FINAL    10000

// =============================================================================
// REACTION DELAYS
// =============================================================================
#define NUM_REACT_DELAYS  3
static const uint16_t REACT_DELAYS[NUM_REACT_DELAYS] = {10000, 15000, 20000};

// =============================================================================
// SHAKE TARGETS
// =============================================================================
#define NUM_SHAKE_TARGETS 3
static const uint8_t SHAKE_TARGETS[NUM_SHAKE_TARGETS] = {10, 15, 20};

// =============================================================================
// GAME STATES
// =============================================================================
typedef enum {
  GAME_IDLE = 0,
  GAME_JOINING,
  GAME_COUNTDOWN,
  GAME_REACTION_WAIT,
  GAME_REACTION_ACTIVE,
  GAME_SHAKE_ACTIVE,
  GAME_RESULTS,
  GAME_FINAL
} GameState;

// =============================================================================
// PLAYER DATA
// =============================================================================
typedef struct {
  bool joined;
  bool finished;
  uint16_t reactionTime;
  uint8_t score;
  uint8_t mac[6];
} Player;

// =============================================================================
// NEOPIXEL
// =============================================================================
typedef enum {
  NEO_OFF = 0,
  NEO_IDLE_RAINBOW,
  NEO_STATUS,
  NEO_RANDOM_FAST,
  NEO_FIXED_COLOR,
  NEO_COUNTDOWN
} NeoMode;

#define NEOPIXEL_COUNT    60
#define LEDS_PER_RING     12
#define NUM_RINGS         5
#define NEO_BRIGHTNESS    50

// Ring mapping: 0=P1, 1=P2, 2=Center, 3=P3, 4=P4
inline uint8_t playerToRing(uint8_t player) {
  return (player < 2) ? player : (player + 1);
}

// =============================================================================
// COLORS (GRB for WS2812B)
// =============================================================================
#define COLOR_OFF         0x000000
#define COLOR_RED         0xFF0000
#define COLOR_GREEN       0x00FF00
#define COLOR_YELLOW      0xFFFF00

// =============================================================================
// NO WINNER INDICATOR
// =============================================================================
#define NO_WINNER         0xFF

#endif // GAMETYPES_H
