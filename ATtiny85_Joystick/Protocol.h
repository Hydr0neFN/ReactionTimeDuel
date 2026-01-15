/*
 * Protocol.h - Reaction Time Duel Communication Protocol
 * Shared definitions for Host, Joysticks, and Display
 * 
 * Packet Format (7 bytes):
 * [START][DEST_ID][SRC_ID][CMD][DATA_HIGH][DATA_LOW][CRC8]
 * 
 * Hardware Signals (directly from ESP32, no UART):
 *   CC1/RST (GPIO18 → PB5): Reset signal
 *   CC2/GO  (GPIO19 → PB2): Game start trigger (shared with SCL!)
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

// =============================================================================
// DEVICE IDS
// =============================================================================
#define ID_HOST       0x00
#define ID_STICK1     0x01
#define ID_STICK2     0x02
#define ID_STICK3     0x03
#define ID_STICK4     0x04
#define ID_DISPLAY    0x05
#define ID_BROADCAST  0xFF

// =============================================================================
// ESP32 HOST PIN ASSIGNMENTS (from schematic)
// =============================================================================
#ifdef ESP32
#define PIN_UART_TX       17    // TX2 → D+ → All device RX
#define PIN_UART_RX       16    // RX2 ← D- ← All device TX (via voltage divider)
#define PIN_RST_OUT       18    // CC1 → RST to all joysticks (PB5)
#define PIN_GO_OUT        19    // CC2 → GO to all joysticks (PB2)
#define PIN_NEOPIXEL      4     // DIN for NeoPixel rings
#define PIN_I2S_DOUT      23    // DINS to MAX98357A
#define PIN_I2S_BCLK      26    // BCLK
#define PIN_I2S_LRC       25    // LRC
// SD Card pins (optional - current build uses SPIFFS instead)
// If SD needed: HSPI pins to avoid conflict with CC1/CC2 on GPIO18/19
#define PIN_SD_CS         5     // Chip Select
#define PIN_SD_SCK        14    // HSPI CLK
#define PIN_SD_MISO       12    // HSPI MISO
#define PIN_SD_MOSI       13    // HSPI MOSI
#endif

// =============================================================================
// ATTINY85 PIN ASSIGNMENTS (from schematic)
// =============================================================================
#ifdef __AVR_ATtiny85__
#define PIN_RX_SDA        0     // PB0: UART RX + I2C SDA (shared!)
#define PIN_TX            1     // PB1: UART TX
#define PIN_SCL_GO        2     // PB2: I2C SCL + GO input (shared!)
#define PIN_BUTTON        3     // PB3: Button input (with hardware debounce)
#define PIN_MOTOR         4     // PB4: Motor PWM output (via Q2)
// PB5 is RESET, directly connected to CC1
#endif

// =============================================================================
// PROTOCOL CONSTANTS
// =============================================================================
#define PACKET_START  0x0A
#define PACKET_SIZE   7

// =============================================================================
// COMMANDS - Host <-> Joystick
// =============================================================================
// From Host
#define CMD_REQ_ID          0x0D  // Manual ID assignment request
#define CMD_ASSIGN_ID       0x20  // Broadcast: assign ID to button presser
#define CMD_GAME_START      0x21  // Game starts (DATA_HIGH=mode, DATA_LOW=param)
#define CMD_TRANSMIT_TOKEN  0x22  // "Player X may transmit" (DATA_LOW=player_id)
#define CMD_VIBRATE         0x23  // 0xFF=GO signal (start timer), else duration×10ms
#define CMD_IDLE            0x24  // Return to idle state
#define CMD_COUNTDOWN       0x25  // Countdown pulse

// From Joystick
#define CMD_OK              0x0B  // ACK / Button pressed during ID assignment
#define CMD_REACTION_DONE   0x26  // Reaction complete (DATA = milliseconds)
#define CMD_SHAKE_DONE      0x27  // Shake complete (DATA = milliseconds)
#define CMD_ERROR           0x0F  // Error response

// =============================================================================
// COMMANDS - Host <-> Display
// =============================================================================
// From Host
#define DISP_IDLE           0x30
#define DISP_PROMPT_JOIN    0x31  // Prompt player to join (DATA_LOW = player_number)
#define DISP_PLAYER_JOINED  0x32  // Player joined (DATA_LOW = player_id)
#define DISP_COUNTDOWN      0x33  // Countdown (DATA_LOW = seconds)
#define DISP_GO             0x34
#define DISP_REACTION_MODE  0x35
#define DISP_SHAKE_MODE     0x36  // DATA_LOW = target (10/15/20)
#define DISP_TIME_P1        0x37  // Player 1 time (DATA_HIGH=time>>8, DATA_LOW=time&0xFF, 0xFFFF=timeout)
#define DISP_TIME_P2        0x38  // Player 2 time
#define DISP_TIME_P3        0x39  // Player 3 time
#define DISP_TIME_P4        0x3A  // Player 4 time
#define DISP_ROUND_WINNER   0x3B  // Round winner (DATA_LOW = player_id, 0 = no winner)
#define DISP_SCORES         0x3C  // Score update (DATA_HIGH=player_id, DATA_LOW=score)
#define DISP_FINAL_WINNER   0x3D  // Final winner (DATA_LOW = player_id)

// From Display
#define TOUCH_SKIP_WAIT     0x40  // User skipped wait

// =============================================================================
// GAME MODES (for CMD_GAME_START DATA_HIGH)
// =============================================================================
#define MODE_REACTION       0x01
#define MODE_SHAKE          0x02

// =============================================================================
// SOUND CATALOG
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
// TIMING CONSTANTS (milliseconds)
// =============================================================================
#define TIMEOUT_JOIN_PHASE    60000   // 60 seconds
#define TIMEOUT_REACTION      10000   // 10 seconds after GO
#define TIMEOUT_SHAKE         30000   // 30 seconds max
#define TIMEOUT_TOKEN_WAIT    100     // Wait for player response
#define DELAY_PACKET          5       // Between packets
#define VIBRATE_COUNTDOWN     200     // Countdown pulse
#define VIBRATE_GO            500     // Game start pulse

// =============================================================================
// GAME CONSTANTS
// =============================================================================
#define MAX_PLAYERS           4
#define TOTAL_ROUNDS          5
#define NEOPIXEL_COUNT        60
#define LEDS_PER_RING         12

// Reaction delays (ms)
#define DELAY_REACT_SHORT     10000
#define DELAY_REACT_MED       15000
#define DELAY_REACT_LONG      20000

// Shake targets (must match available audio)
#define SHAKE_TARGET_LOW      10
#define SHAKE_TARGET_MED      15
#define SHAKE_TARGET_HIGH     20

// =============================================================================
// CRC8 FUNCTION
// =============================================================================
inline uint8_t CRC8(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0x00;
  while (len--) {
    uint8_t extract = *data++;
    for (uint8_t i = 8; i; i--) {
      uint8_t sum = (crc ^ extract) & 0x01;
      crc >>= 1;
      if (sum) {
        crc ^= 0x8C;
      }
      extract >>= 1;
    }
  }
  return crc;
}

// =============================================================================
// PACKET HELPERS
// =============================================================================
typedef struct {
  uint8_t start;
  uint8_t dest_id;
  uint8_t src_id;
  uint8_t cmd;
  uint8_t data_high;
  uint8_t data_low;
  uint8_t crc;
} Packet_t;

// Build 16-bit value from high/low bytes
inline uint16_t buildU16(uint8_t high, uint8_t low) {
  return ((uint16_t)high << 8) | low;
}

// Split 16-bit value into high/low bytes
inline void splitU16(uint16_t val, uint8_t *high, uint8_t *low) {
  *high = (val >> 8) & 0xFF;
  *low = val & 0xFF;
}

#endif // PROTOCOL_H
