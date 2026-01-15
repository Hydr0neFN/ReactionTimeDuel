/*
 * ESP32_HardwareTest.ino - Hardware Assembly Test
 * 
 * Tests: UART, NeoPixel, Audio (2x amp), GO/RST signals
 * 
 * Hardware available:
 *   - ESP32-DEVKIT-V1 (host)
 *   - 1x Joystick (ATtiny85)
 *   - 2x MAX98357A amplifiers
 *   - NeoPixel rings
 *   - NO display
 * 
 * Serial Monitor: 115200 baud
 * Send commands via Serial to trigger tests:
 *   '1' = Test NeoPixels (rainbow)
 *   '2' = Test Audio amp 1 (beep tone)
 *   '3' = Test Audio amp 2 (beep tone)
 *   '4' = Pulse GO signal (joystick should vibrate)
 *   '5' = Pulse RST signal
 *   '6' = Send ping to joystick (test UART)
 *   '7' = Request button state from joystick
 *   '8' = Request shake data from joystick
 *   '9' = All LEDs OFF
 *   '0' = Run full test sequence
 */

#include <Adafruit_NeoPixel.h>

// =============================================================================
// PIN DEFINITIONS (from schematic)
// =============================================================================
#define PIN_UART_TX       17
#define PIN_UART_RX       16
#define PIN_RST           18    // CC1
#define PIN_GO            19    // CC2
#define PIN_NEOPIXEL      4

// I2S for audio (directly driving speaker for test - no MP3)
#define PIN_I2S_DOUT      23
#define PIN_I2S_BCLK      26
#define PIN_I2S_LRC       25

// =============================================================================
// CONFIGURATION
// =============================================================================
#define NEOPIXEL_COUNT    60
#define LEDS_PER_RING     12
#define SERIAL_BAUD       9600

// Protocol constants
#define PACKET_START      0x0A
#define PACKET_SIZE       7
#define ID_HOST           0x00
#define ID_STICK1         0x01
#define ID_BROADCAST      0xFF

// Test commands
#define CMD_PING          0x10
#define CMD_PONG          0x11
#define CMD_GET_BUTTON    0x12
#define CMD_GET_ACCEL     0x13
#define CMD_VIBRATE       0x23

// =============================================================================
// GLOBALS
// =============================================================================
Adafruit_NeoPixel pixels(NEOPIXEL_COUNT, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

uint8_t txPacket[PACKET_SIZE];
uint8_t rxPacket[PACKET_SIZE];

// =============================================================================
// CRC8
// =============================================================================
uint8_t calcCRC(uint8_t *data, uint8_t len) {
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
// PACKET FUNCTIONS
// =============================================================================
void sendPacket(uint8_t dest, uint8_t cmd, uint8_t dataHigh, uint8_t dataLow) {
  txPacket[0] = PACKET_START;
  txPacket[1] = dest;
  txPacket[2] = ID_HOST;
  txPacket[3] = cmd;
  txPacket[4] = dataHigh;
  txPacket[5] = dataLow;
  
  uint8_t crcData[5] = {dest, ID_HOST, cmd, dataHigh, dataLow};
  txPacket[6] = calcCRC(crcData, 5);
  
  Serial2.write(txPacket, PACKET_SIZE);
  Serial2.flush();
  
  Serial.printf("TX → [%02X][%02X][%02X][%02X][%02X][%02X][%02X]\n",
    txPacket[0], txPacket[1], txPacket[2], txPacket[3], 
    txPacket[4], txPacket[5], txPacket[6]);
}

bool receivePacket(unsigned long timeout = 500) {
  unsigned long start = millis();
  
  while (Serial2.available() < PACKET_SIZE) {
    if ((millis() - start) > timeout) {
      Serial.println("RX: Timeout - no response");
      return false;
    }
    delay(1);
  }
  
  while (Serial2.available() >= PACKET_SIZE) {
    if (Serial2.peek() == PACKET_START) {
      for (int i = 0; i < PACKET_SIZE; i++) {
        rxPacket[i] = Serial2.read();
      }
      
      Serial.printf("RX ← [%02X][%02X][%02X][%02X][%02X][%02X][%02X]",
        rxPacket[0], rxPacket[1], rxPacket[2], rxPacket[3], 
        rxPacket[4], rxPacket[5], rxPacket[6]);
      
      // Verify CRC
      uint8_t crcData[5] = {rxPacket[1], rxPacket[2], rxPacket[3], rxPacket[4], rxPacket[5]};
      if (calcCRC(crcData, 5) == rxPacket[6]) {
        Serial.println(" ✓ CRC OK");
        return true;
      } else {
        Serial.println(" ✗ CRC FAIL");
        return false;
      }
    } else {
      Serial2.read();  // Discard
    }
  }
  return false;
}

// =============================================================================
// TEST FUNCTIONS
// =============================================================================
void testNeoPixels() {
  Serial.println("\n=== TEST: NeoPixel Rings ===");
  Serial.println("Running rainbow on all 5 rings...");
  
  for (int cycle = 0; cycle < 256; cycle += 8) {
    for (int i = 0; i < NEOPIXEL_COUNT; i++) {
      int hue = (i * 256 / NEOPIXEL_COUNT + cycle) & 255;
      pixels.setPixelColor(i, pixels.ColorHSV(hue * 256, 255, 100));
    }
    pixels.show();
    delay(20);
  }
  
  // Show each ring separately
  Serial.println("Testing each ring individually:");
  uint32_t colors[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF};
  const char* names[] = {"Ring 0 (P1)", "Ring 1 (P2)", "Ring 2 (Center)", "Ring 3 (P3)", "Ring 4 (P4)"};
  
  for (int ring = 0; ring < 5; ring++) {
    pixels.clear();
    for (int i = 0; i < LEDS_PER_RING; i++) {
      pixels.setPixelColor(ring * LEDS_PER_RING + i, colors[ring]);
    }
    pixels.show();
    Serial.printf("  %s = %s\n", names[ring], 
      ring == 0 ? "RED" : ring == 1 ? "GREEN" : ring == 2 ? "BLUE" : ring == 3 ? "YELLOW" : "MAGENTA");
    delay(1000);
  }
  
  pixels.clear();
  pixels.show();
  Serial.println("NeoPixel test complete.\n");
}

void testAudio(int ampNum) {
  Serial.printf("\n=== TEST: Audio Amplifier %d ===\n", ampNum);
  Serial.println("Generating test tone (square wave)...");
  
  // Simple square wave test tone (no I2S library needed)
  // This tests if the amp is connected and working
  
  pinMode(PIN_I2S_DOUT, OUTPUT);
  pinMode(PIN_I2S_BCLK, OUTPUT);
  pinMode(PIN_I2S_LRC, OUTPUT);
  
  // Generate audible clicks/tone
  for (int i = 0; i < 500; i++) {
    digitalWrite(PIN_I2S_DOUT, HIGH);
    digitalWrite(PIN_I2S_BCLK, HIGH);
    digitalWrite(PIN_I2S_LRC, (i / 50) % 2);
    delayMicroseconds(500);
    digitalWrite(PIN_I2S_DOUT, LOW);
    digitalWrite(PIN_I2S_BCLK, LOW);
    delayMicroseconds(500);
  }
  
  digitalWrite(PIN_I2S_DOUT, LOW);
  digitalWrite(PIN_I2S_BCLK, LOW);
  digitalWrite(PIN_I2S_LRC, LOW);
  
  Serial.println("Did you hear a buzzing tone from the speaker?");
  Serial.println("(Both amps share the same I2S bus, so both should play)\n");
}

void testGOSignal() {
  Serial.println("\n=== TEST: GO Signal (GPIO19) ===");
  Serial.println("Pulsing GO line HIGH for 500ms...");
  Serial.println("Joystick should vibrate if connected!");
  
  digitalWrite(PIN_GO, HIGH);
  delay(500);
  digitalWrite(PIN_GO, LOW);
  
  Serial.println("GO pulse complete.\n");
}

void testRSTSignal() {
  Serial.println("\n=== TEST: RST Signal (GPIO18) ===");
  Serial.println("Pulsing RST line HIGH for 100ms...");
  Serial.println("WARNING: This resets the ATtiny85!");
  
  digitalWrite(PIN_RST, HIGH);
  delay(100);
  digitalWrite(PIN_RST, LOW);
  
  Serial.println("RST pulse complete. Joystick will reboot.\n");
}

void testUARTPing() {
  Serial.println("\n=== TEST: UART Ping to Joystick ===");
  Serial.println("Sending PING to Stick1 (ID 0x01)...");
  
  // Clear RX buffer
  while (Serial2.available()) Serial2.read();
  
  sendPacket(ID_STICK1, CMD_PING, 0x00, 0x00);
  
  if (receivePacket(1000)) {
    if (rxPacket[3] == CMD_PONG) {
      Serial.println("✓ PONG received! UART communication working.\n");
    } else {
      Serial.printf("? Unexpected response CMD: 0x%02X\n", rxPacket[3]);
    }
  } else {
    Serial.println("✗ No response. Check:");
    Serial.println("  - Joystick powered?");
    Serial.println("  - TX/RX wiring correct?");
    Serial.println("  - Joystick running test firmware?\n");
  }
}

void testButtonState() {
  Serial.println("\n=== TEST: Joystick Button State ===");
  Serial.println("Requesting button state...");
  
  while (Serial2.available()) Serial2.read();
  
  sendPacket(ID_STICK1, CMD_GET_BUTTON, 0x00, 0x00);
  
  if (receivePacket(1000)) {
    uint8_t buttonState = rxPacket[5];
    Serial.printf("Button state: %s\n", buttonState ? "PRESSED" : "RELEASED");
    Serial.println("Try pressing/releasing and run test again.\n");
  }
}

void testAccelerometer() {
  Serial.println("\n=== TEST: MPU-6050 Accelerometer ===");
  Serial.println("Requesting accelerometer data...");
  
  while (Serial2.available()) Serial2.read();
  
  sendPacket(ID_STICK1, CMD_GET_ACCEL, 0x00, 0x00);
  
  if (receivePacket(1000)) {
    int16_t magnitude = ((int16_t)rxPacket[4] << 8) | rxPacket[5];
    Serial.printf("Acceleration magnitude: %d\n", magnitude);
    Serial.println("Shake the joystick and run test again.\n");
  }
}

void testVibrateCommand() {
  Serial.println("\n=== TEST: Vibrate via UART Command ===");
  Serial.println("Sending vibrate command (300ms)...");
  
  sendPacket(ID_STICK1, CMD_VIBRATE, 0x00, 30);  // 30 * 10 = 300ms
  
  Serial.println("Joystick should vibrate for 300ms.\n");
}

void ledsOff() {
  pixels.clear();
  pixels.show();
  Serial.println("All LEDs OFF.\n");
}

void runFullTest() {
  Serial.println("\n");
  Serial.println("╔════════════════════════════════════╗");
  Serial.println("║    FULL HARDWARE TEST SEQUENCE     ║");
  Serial.println("╚════════════════════════════════════╝\n");
  
  testNeoPixels();
  delay(500);
  
  testAudio(1);
  delay(500);
  
  testGOSignal();
  delay(500);
  
  testUARTPing();
  delay(500);
  
  testButtonState();
  delay(500);
  
  testAccelerometer();
  delay(500);
  
  testVibrateCommand();
  delay(500);
  
  ledsOff();
  
  Serial.println("╔════════════════════════════════════╗");
  Serial.println("║      TEST SEQUENCE COMPLETE        ║");
  Serial.println("╚════════════════════════════════════╝\n");
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n");
  Serial.println("╔════════════════════════════════════╗");
  Serial.println("║  ESP32 Hardware Test - v1.0        ║");
  Serial.println("║  Reaction Time Duel                ║");
  Serial.println("╚════════════════════════════════════╝\n");
  
  // Initialize UART to joysticks
  Serial2.begin(SERIAL_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);
  Serial.printf("UART2: TX=%d, RX=%d @ %d baud\n", PIN_UART_TX, PIN_UART_RX, SERIAL_BAUD);
  
  // Initialize control signals
  pinMode(PIN_GO, OUTPUT);
  pinMode(PIN_RST, OUTPUT);
  digitalWrite(PIN_GO, LOW);
  digitalWrite(PIN_RST, LOW);
  Serial.printf("Signals: GO=%d, RST=%d\n", PIN_GO, PIN_RST);
  
  // Initialize NeoPixels
  pixels.begin();
  pixels.setBrightness(50);
  pixels.clear();
  pixels.show();
  Serial.printf("NeoPixel: Pin %d, %d LEDs\n", PIN_NEOPIXEL, NEOPIXEL_COUNT);
  
  Serial.println("\n--- Commands ---");
  Serial.println("1 = NeoPixel test");
  Serial.println("2 = Audio test");
  Serial.println("3 = Audio test (same)");
  Serial.println("4 = GO signal (vibrate joystick)");
  Serial.println("5 = RST signal (reset joystick)");
  Serial.println("6 = UART ping test");
  Serial.println("7 = Get button state");
  Serial.println("8 = Get accelerometer");
  Serial.println("9 = LEDs off");
  Serial.println("0 = Full test sequence");
  Serial.println("v = Vibrate via UART command");
  Serial.println("\nReady!\n");
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
  if (Serial.available()) {
    char cmd = Serial.read();
    
    switch (cmd) {
      case '1': testNeoPixels(); break;
      case '2': testAudio(1); break;
      case '3': testAudio(2); break;
      case '4': testGOSignal(); break;
      case '5': testRSTSignal(); break;
      case '6': testUARTPing(); break;
      case '7': testButtonState(); break;
      case '8': testAccelerometer(); break;
      case '9': ledsOff(); break;
      case '0': runFullTest(); break;
      case 'v': case 'V': testVibrateCommand(); break;
      case '\n': case '\r': break;  // Ignore newlines
      default:
        Serial.printf("Unknown command: '%c'\n", cmd);
        break;
    }
  }
  
  // Check for unsolicited data from joystick
  if (Serial2.available() >= PACKET_SIZE) {
    Serial.println("Unsolicited data from joystick:");
    receivePacket(100);
  }
  
  delay(10);
}
