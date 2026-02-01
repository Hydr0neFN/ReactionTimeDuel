/*
 * DisplayProtocol.h - Protocol Reference for ESP32-S3 Display
 * Reaction Time Duel
 * 
 * UART: 9600 baud, 8N1
 * Shared bus with joysticks (D+/D- lines)
 * 
 * Display ID: 0x05
 * Host ID: 0x00
 */

#ifndef DISPLAY_PROTOCOL_H
#define DISPLAY_PROTOCOL_H

#include <Arduino.h>

// =============================================================================
// PROTOCOL CONSTANTS
// =============================================================================
#define PACKET_START      0x0A
#define PACKET_SIZE       7

#define ID_HOST           0x00
#define ID_DISPLAY        0x05
#define ID_BROADCAST      0xFF

// =============================================================================
// COMMANDS FROM HOST (you receive these)
// =============================================================================
#define DISP_IDLE           0x30  // Show idle/start screen
#define DISP_PROMPT_JOIN    0x31  // DATA_LOW: 0 = generic "press to join", 1-4 = prompt specific player
#define DISP_PLAYER_JOINED  0x32  // DATA_LOW = player (1-4) joined
#define DISP_COUNTDOWN      0x33  // DATA_LOW = seconds (3, 2, 1)
#define DISP_GO             0x34  // Show "GO!"
#define DISP_REACTION_MODE  0x35  // Reaction mode selected
#define DISP_SHAKE_MODE     0x36  // DATA_LOW = shake target (10, 15, 20)
#define DISP_TIME_P1        0x37  // Player 1 time: DATA_HIGH<<8 | DATA_LOW = ms (0xFFFF = timeout/penalty)
#define DISP_TIME_P2        0x38  // Player 2 time (0xFFFF = show red ring, no time)
#define DISP_TIME_P3        0x39  // Player 3 time
#define DISP_TIME_P4        0x3A  // Player 4 time
#define DISP_ROUND_WINNER   0x3B  // DATA_LOW = winner (1-4), 0 = no winner
#define DISP_SCORES         0x3C  // DATA_HIGH = player (1-4), DATA_LOW = score
#define DISP_FINAL_WINNER   0x3D  // DATA_LOW = winner (1-4)

// =============================================================================
// COMMANDS TO HOST (you send these)
// =============================================================================
#define TOUCH_SKIP_WAIT     0x40  // User touched screen to skip wait

// =============================================================================
// PACKET STRUCTURE
// =============================================================================
/*
 * 7 bytes total:
 * [0] START     = 0x0A (always)
 * [1] DEST_ID   = destination device
 * [2] SRC_ID    = sender device
 * [3] CMD       = command byte
 * [4] DATA_HIGH = high byte of data
 * [5] DATA_LOW  = low byte of data
 * [6] CRC       = CRC8 of bytes [1]-[5]
 */

// =============================================================================
// CRC8 FUNCTION
// =============================================================================
uint8_t calcCRC8(const uint8_t *data, uint8_t len) {
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
// RECEIVE PACKET (call in loop)
// =============================================================================
// Returns true if valid packet received for display
// Packet data stored in provided buffer

bool receivePacket(HardwareSerial &serial, uint8_t *packet) {
  if (serial.available() < PACKET_SIZE) return false;
  
  while (serial.available() >= PACKET_SIZE) {
    if (serial.peek() == PACKET_START) {
      // Read full packet
      for (int i = 0; i < PACKET_SIZE; i++) {
        packet[i] = serial.read();
      }
      
      // Check if for us (display or broadcast)
      uint8_t dest = packet[1];
      if (dest == ID_DISPLAY || dest == ID_BROADCAST) {
        // Verify CRC
        uint8_t crcData[5] = {packet[1], packet[2], packet[3], packet[4], packet[5]};
        if (calcCRC8(crcData, 5) == packet[6]) {
          return true;  // Valid packet!
        }
      }
      return false;  // Not for us or bad CRC
    } else {
      serial.read();  // Discard non-start byte
    }
  }
  return false;
}

// =============================================================================
// SEND PACKET (to host)
// =============================================================================
void sendPacket(HardwareSerial &serial, uint8_t cmd, uint8_t dataHigh, uint8_t dataLow) {
  uint8_t packet[PACKET_SIZE];
  
  packet[0] = PACKET_START;
  packet[1] = ID_HOST;       // Destination = Host
  packet[2] = ID_DISPLAY;    // Source = Display
  packet[3] = cmd;
  packet[4] = dataHigh;
  packet[5] = dataLow;
  
  uint8_t crcData[5] = {packet[1], packet[2], packet[3], packet[4], packet[5]};
  packet[6] = calcCRC8(crcData, 5);
  
  serial.write(packet, PACKET_SIZE);
  serial.flush();
  delay(5);
}

// =============================================================================
// HELPER: Extract 16-bit time from packet
// =============================================================================
uint16_t getTimeFromPacket(uint8_t *packet) {
  return ((uint16_t)packet[4] << 8) | packet[5];
}

// =============================================================================
// EXAMPLE USAGE
// =============================================================================
/*

// Setup
HardwareSerial DisplaySerial(1);  // Use UART1, adjust pins as needed
uint8_t rxPacket[7];

void setup() {
  DisplaySerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);
}

void loop() {
  if (receivePacket(DisplaySerial, rxPacket)) {
    uint8_t cmd = rxPacket[3];
    uint8_t dataHigh = rxPacket[4];
    uint8_t dataLow = rxPacket[5];
    
    switch (cmd) {
      case DISP_IDLE:
        // Show start screen with "START" button
        break;
        
      case DISP_PROMPT_JOIN:
        // dataLow = 0: Generic "Press button to join" for all
        // dataLow = 1-4: Prompt specific player
        if (dataLow == 0) {
          // Show generic join prompt
        } else {
          // Highlight player circle for dataLow
        }
        break;
        
      case DISP_PLAYER_JOINED:
        // Change player circle to green
        // dataLow = player number (1-4)
        break;
        
      case DISP_COUNTDOWN:
        // Show countdown number
        // dataLow = seconds (3, 2, 1)
        break;
        
      case DISP_GO:
        // Show "GO!" on screen
        break;
        
      case DISP_REACTION_MODE:
        // Show "Reaction Mode" indicator
        break;
        
      case DISP_SHAKE_MODE:
        // Show "Shake Mode" + target
        // dataLow = target (10, 15, or 20)
        break;
        
      case DISP_TIME_P1:
      case DISP_TIME_P2:
      case DISP_TIME_P3:
      case DISP_TIME_P4: {
        uint8_t player = cmd - DISP_TIME_P1 + 1;  // 1-4
        uint16_t timeMs = getTimeFromPacket(rxPacket);
        if (timeMs == 0xFFFF) {
          // Timeout OR Penalty (cheater) - show red ring, no time
          // Could display "FOUL" or "---" instead of time
        } else {
          // Show time: e.g. "245 ms"
        }
        break;
      }
        
      case DISP_ROUND_WINNER:
        // Show "WINNER - PLAYER X"
        // dataLow = winner (1-4), or 0 = no winner
        break;
        
      case DISP_SCORES:
        // Update score display
        // dataHigh = player (1-4)
        // dataLow = score
        break;
        
      case DISP_FINAL_WINNER:
        // Show final winner screen
        // dataLow = winner (1-4)
        break;
    }
  }
  
  // Example: Send skip when user touches screen
  if (screenTouched) {
    sendPacket(DisplaySerial, TOUCH_SKIP_WAIT, 0x00, 0x00);
  }
}

*/

#endif // DISPLAY_PROTOCOL_H
