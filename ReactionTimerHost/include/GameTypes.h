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
#define DURATION_FINAL    15000

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
  NEO_COUNTDOWN,
  NEO_BLINK_SLOT,       // Blink specific player slot ring during join
  NEO_SHAKE_COUNTDOWN   // Shake mode: random player rings + center countdown
} NeoMode;

// Center ring countdown: 12 LEDs over 30 seconds = 2500ms per LED
#define CENTER_RING       2
#define SHAKE_LED_INTERVAL  (TIMEOUT_SHAKE / LEDS_PER_RING)  // 2500ms

#define NEOPIXEL_COUNT    60
#define LEDS_PER_RING     12
#define NUM_RINGS         5
#define NEO_BRIGHTNESS    50

// Ring mapping: Player order left-to-right: P1=Ring4, P2=Ring3, P3=Ring1, P4=Ring0
inline uint8_t playerToRing(uint8_t player) {
  // Reverse order: player 0->ring 4, player 1->ring 3, player 2->ring 1, player 3->ring 0
  static const uint8_t mapping[4] = {4, 3, 1, 0};
  return mapping[player & 0x03];
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
