/*
 * Protocol.h - Reaction Time Duel Communication Protocol
 * Shared between ESP32-S3 Master and ESP8266 Joysticks
 *
 * Packet Format (7 bytes):
 * [START][DEST_ID][SRC_ID][CMD][DATA_HIGH][DATA_LOW][CRC8]
 *
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

// =============================================================================
// FIRMWARE VERSION — increment on every code change
// Encoded in CMD_REQ_ID: data_high = (MAJOR<<4)|MINOR, data_low = PATCH
// =============================================================================
#define FW_VERSION_MAJOR  3
#define FW_VERSION_MINOR  0
#define FW_VERSION_PATCH  0
#define FW_VERSION_STRING "V3.0.0"

// =============================================================================
// PACKET STRUCTURE
// =============================================================================
#define PACKET_START      0x0A
#define PACKET_SIZE       7

typedef struct __attribute__((packed)) {
  uint8_t start;
  uint8_t dest_id;
  uint8_t src_id;
  uint8_t cmd;
  uint8_t data_high;
  uint8_t data_low;
  uint8_t crc;
} GamePacket;

// =============================================================================
// DEVICE IDS
// =============================================================================
#define ID_HOST           0x00
#define ID_STICK1         0x01
#define ID_STICK2         0x02
#define ID_STICK3         0x03
#define ID_STICK4         0x04
#define ID_BROADCAST      0xFF

// =============================================================================
// COMMANDS: Host → Joysticks
// =============================================================================
#define CMD_OK            0x0B  // Acknowledge (join confirmed)
#define CMD_ACK           0x0E  // ACK: data_low = cmd being acknowledged
#define CMD_GAME_START    0x21  // Start round (data_high=mode, data_low=param)
#define CMD_GO            0x22  // GO signal - start timing now!
#define CMD_VIBRATE       0x23  // Vibrate (0xFF=GO signal, else duration×10ms)
#define CMD_IDLE          0x24  // Return to idle state
#define CMD_COUNTDOWN     0x25  // Countdown tick (data_low = 3, 2, or 1)

// =============================================================================
// COMMANDS: Joysticks → Host
// =============================================================================
#define CMD_REQ_ID        0x0D  // Request to join game
#define CMD_REACTION_DONE 0x26  // Reaction complete (data = time_ms, 0xFFFF=penalty)
#define CMD_SHAKE_DONE    0x27  // Shake complete (data = time_ms, 0xFFFF=timeout)
#define CMD_SHAKE_PROGRESS 0x28 // Shake progress (data_high=count, data_low=target) — sent every 5 shakes

// =============================================================================
// GAME MODES
// =============================================================================
#define MODE_REACTION     0x01
#define MODE_SHAKE        0x02

// =============================================================================
// ESP-NOW CONFIGURATION
// =============================================================================
#define ESPNOW_CHANNEL    6

// =============================================================================
// SPECIAL VALUES
// =============================================================================
#define TIME_PENALTY      0xFFFF  // Timeout or early press
#define VIBRATE_GO        0xFF    // GO signal vibration

// =============================================================================
// CRC8 CALCULATION (Polynomial 0x8C)
// =============================================================================
inline uint8_t calcCRC8(const uint8_t* data, uint8_t len) {
  uint8_t crc = 0x00;
  while (len--) {
    uint8_t extract = *data++;
    for (uint8_t i = 8; i; i--) {
      uint8_t sum = (crc ^ extract) & 0x01;
      crc >>= 1;
      if (sum) crc ^= 0x8C;
      extract >>= 1;
    }
  }
  return crc;
}

// =============================================================================
// PACKET HELPERS
// =============================================================================
inline uint16_t packetData(const GamePacket* pkt) {
  return ((uint16_t)pkt->data_high << 8) | pkt->data_low;
}

inline void setPacketData(GamePacket* pkt, uint16_t data) {
  pkt->data_high = (data >> 8) & 0xFF;
  pkt->data_low = data & 0xFF;
}

inline bool validatePacket(const GamePacket* pkt) {
  if (pkt->start != PACKET_START) return false;
  return calcCRC8((const uint8_t*)pkt, 6) == pkt->crc;
}

inline void buildPacket(GamePacket* pkt, uint8_t dest, uint8_t src, uint8_t cmd, uint16_t data) {
  pkt->start = PACKET_START;
  pkt->dest_id = dest;
  pkt->src_id = src;
  pkt->cmd = cmd;
  pkt->data_high = (data >> 8) & 0xFF;
  pkt->data_low = data & 0xFF;
  pkt->crc = calcCRC8((const uint8_t*)pkt, 6);
}

#endif // PROTOCOL_H
