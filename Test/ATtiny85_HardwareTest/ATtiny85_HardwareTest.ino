/*
 * ATtiny85_HardwareTest.ino - Joystick Hardware Test
 * 
 * Tests: UART, Button, Motor, MPU-6050, GO signal
 * 
 * Responds to test commands from ESP32_HardwareTest.ino
 * Also runs self-test on boot.
 * 
 * Pin Assignments:
 *   PB0 (pin 5): UART RX + I2C SDA
 *   PB1 (pin 6): UART TX
 *   PB2 (pin 7): I2C SCL + GO signal input
 *   PB3 (pin 2): Button (INPUT_PULLUP)
 *   PB4 (pin 3): Motor PWM
 *   PB5 (pin 1): RESET
 */

#include <SoftwareSerial.h>

// =============================================================================
// PIN DEFINITIONS
// =============================================================================
#define PIN_RX      0     // PB0 (shared with SDA)
#define PIN_TX      1     // PB1 (shared with Motor)
#define PIN_MOTOR   1     // PB1 - drives transistor Q2 via R6
#define PIN_SCL     2     // PB2
#define PIN_GO      3     // PB3
#define PIN_BUTTON  4     // PB4

// =============================================================================
// PROTOCOL
// =============================================================================
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
// MPU-6050 (Software I2C)
// =============================================================================
#define MPU_ADDR          0x68
#define MPU_PWR_MGMT_1    0x6B
#define MPU_ACCEL_XOUT_H  0x3B

#define SDA_PIN   0
#define SCL_PIN   2
#define I2C_DELAY 5

// Safe macros for use in if/else (do-while wrapper)
#define SDA_HIGH()  do { pinMode(SDA_PIN, INPUT); } while(0)
#define SDA_LOW()   do { pinMode(SDA_PIN, OUTPUT); digitalWrite(SDA_PIN, LOW); } while(0)
#define SCL_HIGH()  do { pinMode(SCL_PIN, INPUT); } while(0)
#define SCL_LOW()   do { pinMode(SCL_PIN, OUTPUT); digitalWrite(SCL_PIN, LOW); } while(0)
#define SDA_READ()  digitalRead(SDA_PIN)

// =============================================================================
// GLOBALS
// =============================================================================
SoftwareSerial uart(PIN_RX, PIN_TX);

uint8_t selfID = ID_STICK1;  // Fixed ID for testing
uint8_t rxBuffer[PACKET_SIZE];
uint8_t txBuffer[PACKET_SIZE];

bool mpuOK = false;

// =============================================================================
// CRC8
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
// SOFTWARE I2C
// =============================================================================
void i2cStart() {
  SDA_HIGH(); SCL_HIGH(); delayMicroseconds(I2C_DELAY);
  SDA_LOW(); delayMicroseconds(I2C_DELAY);
  SCL_LOW(); delayMicroseconds(I2C_DELAY);
}

void i2cStop() {
  SDA_LOW(); SCL_HIGH(); delayMicroseconds(I2C_DELAY);
  SDA_HIGH(); delayMicroseconds(I2C_DELAY);
}

bool i2cWriteByte(uint8_t data) {
  for (uint8_t i = 0; i < 8; i++) {
    if (data & 0x80) SDA_HIGH(); else SDA_LOW();
    data <<= 1;
    SCL_HIGH(); delayMicroseconds(I2C_DELAY);
    SCL_LOW(); delayMicroseconds(I2C_DELAY);
  }
  SDA_HIGH();
  SCL_HIGH(); delayMicroseconds(I2C_DELAY);
  bool ack = !SDA_READ();
  SCL_LOW(); delayMicroseconds(I2C_DELAY);
  return ack;
}

uint8_t i2cReadByte(bool ack) {
  uint8_t data = 0;
  SDA_HIGH();
  for (uint8_t i = 0; i < 8; i++) {
    data <<= 1;
    SCL_HIGH(); delayMicroseconds(I2C_DELAY);
    if (SDA_READ()) data |= 1;
    SCL_LOW(); delayMicroseconds(I2C_DELAY);
  }
  if (ack) SDA_LOW(); else SDA_HIGH();
  SCL_HIGH(); delayMicroseconds(I2C_DELAY);
  SCL_LOW(); delayMicroseconds(I2C_DELAY);
  SDA_HIGH();
  return data;
}

bool initMPU() {
  i2cStart();
  bool ok = i2cWriteByte(MPU_ADDR << 1);
  if (ok) {
    i2cWriteByte(MPU_PWR_MGMT_1);
    i2cWriteByte(0x00);  // Wake up
  }
  i2cStop();
  return ok;
}

uint16_t readAccelMagnitude() {
  int16_t ax, ay, az;
  
  i2cStart();
  if (!i2cWriteByte(MPU_ADDR << 1)) { i2cStop(); return 0; }
  i2cWriteByte(MPU_ACCEL_XOUT_H);
  i2cStop();
  
  i2cStart();
  if (!i2cWriteByte((MPU_ADDR << 1) | 1)) { i2cStop(); return 0; }
  ax = (i2cReadByte(true) << 8) | i2cReadByte(true);
  ay = (i2cReadByte(true) << 8) | i2cReadByte(true);
  az = (i2cReadByte(true) << 8) | i2cReadByte(false);
  i2cStop();
  
  // Return simple magnitude sum
  return (uint16_t)(abs(ax)/100 + abs(ay)/100 + abs(az)/100);
}

// =============================================================================
// MOTOR
// =============================================================================
void vibrateMotor(uint16_t ms) {
  analogWrite(PIN_MOTOR, 200);
  delay(ms);
  analogWrite(PIN_MOTOR, 0);
}

// =============================================================================
// PACKET FUNCTIONS
// =============================================================================
void sendPacket(uint8_t dest, uint8_t cmd, uint8_t dataHigh, uint8_t dataLow) {
  txBuffer[0] = PACKET_START;
  txBuffer[1] = dest;
  txBuffer[2] = selfID;
  txBuffer[3] = cmd;
  txBuffer[4] = dataHigh;
  txBuffer[5] = dataLow;
  
  uint8_t crcData[5] = {dest, selfID, cmd, dataHigh, dataLow};
  txBuffer[6] = calcCRC8(crcData, 5);
  
  uart.write(txBuffer, PACKET_SIZE);
  delay(5);
}

bool receivePacket() {
  if (uart.available() < PACKET_SIZE) return false;
  
  while (uart.available() >= PACKET_SIZE) {
    if (uart.peek() == PACKET_START) {
      for (uint8_t i = 0; i < PACKET_SIZE; i++) {
        rxBuffer[i] = uart.read();
      }
      
      uint8_t dest = rxBuffer[1];
      if (dest == selfID || dest == ID_BROADCAST) {
        uint8_t crcData[5] = {rxBuffer[1], rxBuffer[2], rxBuffer[3], rxBuffer[4], rxBuffer[5]};
        if (calcCRC8(crcData, 5) == rxBuffer[6]) {
          return true;
        }
      }
      return false;
    } else {
      uart.read();
    }
  }
  return false;
}

// =============================================================================
// COMMAND HANDLER
// =============================================================================
void handleCommand() {
  uint8_t cmd = rxBuffer[3];
  uint8_t dataLow = rxBuffer[5];
  
  switch (cmd) {
    case CMD_PING:
      // Respond with PONG
      sendPacket(ID_HOST, CMD_PONG, 0x00, 0x00);
      vibrateMotor(50);  // Quick buzz to confirm
      break;
      
    case CMD_GET_BUTTON:
      // Send button state (0 = released, 1 = pressed)
      {
        uint8_t state = (digitalRead(PIN_BUTTON) == LOW) ? 1 : 0;
        sendPacket(ID_HOST, CMD_GET_BUTTON, 0x00, state);
      }
      break;
      
    case CMD_GET_ACCEL:
      // Send accelerometer magnitude
      if (mpuOK) {
        uint16_t mag = readAccelMagnitude();
        sendPacket(ID_HOST, CMD_GET_ACCEL, (mag >> 8) & 0xFF, mag & 0xFF);
      } else {
        sendPacket(ID_HOST, CMD_GET_ACCEL, 0xFF, 0xFF);  // Error
      }
      break;
      
    case CMD_VIBRATE:
      // Vibrate for specified duration
      if (dataLow == 0xFF) {
        vibrateMotor(500);  // GO signal
      } else {
        vibrateMotor((uint16_t)dataLow * 10);
      }
      break;
  }
}

// =============================================================================
// SELF TEST (runs on boot)
// =============================================================================
void selfTest() {
  // 1. Motor test - 3 short buzzes
  for (int i = 0; i < 3; i++) {
    vibrateMotor(100);
    delay(100);
  }
  
  delay(500);
  
  // 2. MPU test
  mpuOK = initMPU();
  if (mpuOK) {
    vibrateMotor(300);  // Long buzz = MPU OK
  } else {
    // Short-short = MPU fail
    vibrateMotor(50);
    delay(100);
    vibrateMotor(50);
  }
  
  delay(500);
  
  // 3. Button test - vibrate while held
  // (User can test by holding button during boot)
  if (digitalRead(PIN_BUTTON) == LOW) {
    while (digitalRead(PIN_BUTTON) == LOW) {
      vibrateMotor(50);
      delay(50);
    }
  }
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_MOTOR, OUTPUT);
  digitalWrite(PIN_MOTOR, LOW);
  pinMode(PIN_GO, INPUT);
  
  uart.begin(9600);
  
  // Run self-test
  selfTest();
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
  // Check for hardware GO signal
  static bool lastGO = false;
  bool currentGO = (digitalRead(PIN_GO) == HIGH);
  if (currentGO && !lastGO) {
    // Rising edge on GO pin
    vibrateMotor(500);
  }
  lastGO = currentGO;
  
  // Handle UART commands
  if (receivePacket()) {
    handleCommand();
  }
  
  // Button feedback (for manual testing)
  static bool lastButton = false;
  bool currentButton = (digitalRead(PIN_BUTTON) == LOW);
  if (currentButton && !lastButton) {
    vibrateMotor(50);  // Quick feedback on press
  }
  lastButton = currentButton;
  
  delay(1);
}
