/*
 * GameTypes.h - Joystick Game Constants
 * ESP8266 Joystick only (Host has its own extended version)
 */

#ifndef GAMETYPES_H
#define GAMETYPES_H

#include <stdint.h>

// =============================================================================
// TIMING (milliseconds) — must match Host values
// =============================================================================
#define TIMEOUT_REACTION  5000    // Max time after GO before penalty (must match Host)
#define TIMEOUT_SHAKE     30000   // Max shake phase before penalty

#endif // GAMETYPES_H
