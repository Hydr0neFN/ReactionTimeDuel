/*
 * ATtiny85_Joystick.ino - Reaction Time Duel Joystick Controller
 * 
 * Hardware: ATtiny85-20P + MPU-6050 + Button + Vibration Motor
 * 
 * Pin Assignments (from schematic):
 *   PB0 (pin 5): UART RX + I2C SDA (SHARED!)
 *   PB1 (pin 6): UART TX
 *   PB2 (pin 7): I2C SCL + GO signal input (SHARED!)
 *   PB3 (pin 2): Button (INPUT_PULLUP, hardware debounce via R5/C4)
 *   PB4 (pin 3): Vibration Motor PWM (via Q2 transistor)
 *   PB5 (pin 1): RESET (directly connected to CC1/RST from host)
 * 
 * Shared Pin Strategy:
 *   - I2C (MPU) only used during SHAKE_MODE
 *   - UART active during all other phases
 *   - GO signal read as digital HIGH on PB2 before I2C operations
 * 
 * Memory Budget: Target <6KB flash
 */

#include <SoftwareSerial.h>

// =============================================================================
// PIN DEFINITIONS (from schematic)
// =============================================================================
#define PIN_RX      0     // PB0: UART RX (shared with SDA)
#define PIN_TX      1     // PB1: UART TX
#define PIN_GO      2     // PB2: GO signal input (shared with SCL)
#define PIN_BUTTON  3     // PB3: Button
#define PIN_MOTOR   4     // PB4: Motor PWM

// =============================================================================
// PROTOCOL CONSTANTS (minimal - no include to save flash)
// =============================================================================
#define PACKET_START      0x0A
#define PACKET_SIZE       7

#define ID_HOST           0x00
#define ID_BROADCAST      0xFF

// Commands from Host
#define CMD_ASSIGN_ID       0x20
#define CMD_GAME_START      0x21
#define CMD_TRANSMIT_TOKEN  0x22
#define CMD_VIBRATE         0x23
#define CMD_IDLE            0x24
#define CMD_COUNTDOWN       0x25

// Commands from Joystick
#define CMD_OK              0x0B
#define CMD_REACTION_DONE   0x26
#define CMD_SHAKE_DONE      0x27

// Game modes
#define MODE_REACTION       0x01
#define MODE_SHAKE          0x02

// =============================================================================
// MPU-6050 (Software I2C on PB0=SDA, PB2=SCL)
// =============================================================================
#define MPU_ADDR          0x68
#define MPU_PWR_MGMT_1    0x6B
#define MPU_ACCEL_XOUT_H  0x3B

// Software I2C macros (avoid library overhead)
#define SDA_PIN   0   // PB0
#define SCL_PIN   2   // PB2
#define I2C_DELAY 5

#define SDA_HIGH()  pinMode(SDA_PIN, INPUT)
#define SDA_LOW()   pinMode(SDA_PIN, OUTPUT); digitalWrite(SDA_PIN, LOW)
#define SCL_HIGH()  pinMode(SCL_PIN, INPUT)
#define SCL_LOW()   pinMode(SCL_PIN, OUTPUT); digitalWrite(SCL_PIN, LOW)
#define SDA_READ()  digitalRead(SDA_PIN)

// =============================================================================
// CONFIGURATION
// =============================================================================
#define BAUD_RATE         9600
#define SHAKE_THRESHOLD   20000   // Acceleration sum threshold
#define SHAKE_DEBOUNCE    150     // ms between shake counts

// =============================================================================
// STATE MACHINE
// =============================================================================
enum State : uint8_t {
  STATE_UNASSIGNED,
  STATE_IDLE,
  STATE_WAITING_GO,
  STATE_BUTTON_MODE,
  STATE_SHAKE_MODE,
  STATE_FINISHED
};

// =============================================================================
// GLOBALS (minimize RAM usage)
// =============================================================================
SoftwareSerial uart(PIN_RX, PIN_TX);

uint8_t selfID = 0x00;
State currentState = STATE_UNASSIGNED;

uint8_t rxBuffer[PACKET_SIZE];
uint8_t txBuffer[PACKET_SIZE];

unsigned long gameStartTime = 0;
unsigned long lastShakeTime = 0;
uint16_t resultTime = 0;
uint8_t shakeCount = 0;
uint8_t shakeTarget = 0;
uint8_t gameMode = 0;

// =============================================================================
// CRC8 CALCULATION
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
// SOFTWARE I2C (minimal implementation for MPU-6050)
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
  // ACK
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
  // Send ACK/NACK
  if (ack) SDA_LOW(); else SDA_HIGH();
  SCL_HIGH(); delayMicroseconds(I2C_DELAY);
  SCL_LOW(); delayMicroseconds(I2C_DELAY);
  SDA_HIGH();
  return data;
}

void initMPU() {
  // Wake up MPU-6050
  i2cStart();
  i2cWriteByte(MPU_ADDR << 1);      // Write address
  i2cWriteByte(MPU_PWR_MGMT_1);     // Register
  i2cWriteByte(0x00);               // Wake up
  i2cStop();
}

bool readAccel(int16_t *ax, int16_t *ay, int16_t *az) {
  i2cStart();
  if (!i2cWriteByte(MPU_ADDR << 1)) { i2cStop(); return false; }
  i2cWriteByte(MPU_ACCEL_XOUT_H);
  i2cStop();
  
  i2cStart();
  if (!i2cWriteByte((MPU_ADDR << 1) | 1)) { i2cStop(); return false; }
  *ax = (i2cReadByte(true) << 8) | i2cReadByte(true);
  *ay = (i2cReadByte(true) << 8) | i2cReadByte(true);
  *az = (i2cReadByte(true) << 8) | i2cReadByte(false);
  i2cStop();
  return true;
}

// =============================================================================
// MOTOR CONTROL
// =============================================================================
void vibrateMotor(uint16_t durationMs) {
  analogWrite(PIN_MOTOR, 200);
  delay(durationMs);
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
      
      // Check if packet is for us
      uint8_t dest = rxBuffer[1];
      if (dest == selfID || dest == ID_BROADCAST || 
          (selfID == 0x00 && dest == 0x00)) {
        // Verify CRC
        uint8_t crcData[5] = {rxBuffer[1], rxBuffer[2], rxBuffer[3], rxBuffer[4], rxBuffer[5]};
        if (calcCRC8(crcData, 5) == rxBuffer[6]) {
          return true;
        }
      }
      return false;
    } else {
      uart.read();  // Discard
    }
  }
  return false;
}

// =============================================================================
// COMMAND HANDLER
// =============================================================================
void handleCommand() {
  uint8_t cmd = rxBuffer[3];
  uint8_t dataHigh = rxBuffer[4];
  uint8_t dataLow = rxBuffer[5];
  
  switch (cmd) {
    case CMD_ASSIGN_ID:
      // Claim ID if button is pressed
      if (currentState == STATE_UNASSIGNED) {
        if (digitalRead(PIN_BUTTON) == LOW) {
          selfID = dataLow;
          sendPacket(ID_HOST, CMD_OK, 0x00, selfID);
          currentState = STATE_IDLE;
          vibrateMotor(100);
        }
      }
      break;
      
    case CMD_IDLE:
      currentState = STATE_IDLE;
      shakeCount = 0;
      resultTime = 0;
      gameMode = 0;
      break;
      
    case CMD_GAME_START:
      gameMode = dataHigh;
      shakeTarget = dataLow;
      shakeCount = 0;
      resultTime = 0;
      currentState = STATE_WAITING_GO;
      
      // Initialize MPU if shake mode
      if (gameMode == MODE_SHAKE) {
        initMPU();
      }
      break;
      
    case CMD_COUNTDOWN:
      vibrateMotor(200);
      break;
      
    case CMD_VIBRATE:
      if (dataLow == 0xFF) {
        // GO signal! Record start time
        gameStartTime = millis();
        vibrateMotor(500);
        
        if (gameMode == MODE_SHAKE) {
          currentState = STATE_SHAKE_MODE;
        } else {
          currentState = STATE_BUTTON_MODE;
        }
      } else {
        vibrateMotor((uint16_t)dataLow * 10);
      }
      break;
      
    case CMD_TRANSMIT_TOKEN:
      if (dataLow == selfID && currentState == STATE_FINISHED) {
        // Send our result
        uint8_t timeHigh = (resultTime >> 8) & 0xFF;
        uint8_t timeLow = resultTime & 0xFF;
        
        uint8_t respCmd = (gameMode == MODE_SHAKE) ? CMD_SHAKE_DONE : CMD_REACTION_DONE;
        sendPacket(ID_HOST, respCmd, timeHigh, timeLow);
        
        currentState = STATE_IDLE;
      }
      break;
  }
}

// =============================================================================
// SHAKE DETECTION
// =============================================================================
bool detectShake() {
  int16_t ax, ay, az;
  if (!readAccel(&ax, &ay, &az)) return false;
  
  // Simple magnitude (avoid sqrt)
  uint32_t mag = (uint32_t)abs(ax) + (uint32_t)abs(ay) + (uint32_t)abs(az);
  
  unsigned long now = millis();
  if (mag > SHAKE_THRESHOLD && (now - lastShakeTime) > SHAKE_DEBOUNCE) {
    lastShakeTime = now;
    return true;
  }
  return false;
}

// =============================================================================
// GO SIGNAL CHECK (hardware signal on PB2)
// =============================================================================
bool checkGoSignal() {
  // GO signal is HIGH when host triggers
  // Only check when not doing I2C (PB2 used as input)
  pinMode(PIN_GO, INPUT);
  return digitalRead(PIN_GO) == HIGH;
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_MOTOR, OUTPUT);
  digitalWrite(PIN_MOTOR, LOW);
  pinMode(PIN_GO, INPUT);
  
  uart.begin(BAUD_RATE);
  
  // Power-on indication
  vibrateMotor(100);
  
  currentState = STATE_UNASSIGNED;
}

// =============================================================================
// MAIN LOOP
// =============================================================================
void loop() {
  // Handle UART commands (when not in shake mode)
  if (currentState != STATE_SHAKE_MODE) {
    if (receivePacket()) {
      handleCommand();
    }
  }
  
  // State logic
  switch (currentState) {
    case STATE_UNASSIGNED:
    case STATE_IDLE:
      // Just wait for commands
      break;
      
    case STATE_WAITING_GO:
      // Check for hardware GO signal as backup
      // (main trigger is CMD_VIBRATE with 0xFF)
      if (checkGoSignal() && gameStartTime == 0) {
        gameStartTime = millis();
        vibrateMotor(500);
        currentState = (gameMode == MODE_SHAKE) ? STATE_SHAKE_MODE : STATE_BUTTON_MODE;
      }
      break;
      
    case STATE_BUTTON_MODE:
      if (digitalRead(PIN_BUTTON) == LOW) {
        resultTime = (uint16_t)(millis() - gameStartTime);
        currentState = STATE_FINISHED;
        vibrateMotor(100);
      }
      break;
      
    case STATE_SHAKE_MODE:
      if (detectShake()) {
        shakeCount++;
        vibrateMotor(50);  // Brief feedback
        
        if (shakeCount >= shakeTarget) {
          resultTime = (uint16_t)(millis() - gameStartTime);
          currentState = STATE_FINISHED;
          vibrateMotor(200);
        }
      }
      
      // Also check for UART (token) between MPU reads
      if (uart.available() >= PACKET_SIZE) {
        if (receivePacket()) {
          handleCommand();
        }
      }
      break;
      
    case STATE_FINISHED:
      // Wait for transmit token
      break;
  }
  
  delay(1);
}
