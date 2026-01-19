/*
 * ESP8266_Slave.ino - Reaction Time Duel - Joystick Controller
 * 
 * Hardware: ESP-12F / NodeMCU / D1 Mini
 * Functions: Button input, Vibration motor, MPU-6050 shake detection
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <Wire.h>

#include "Protocol.h"

// =============================================================================
// CONFIGURATION - CHANGE FOR EACH JOYSTICK!
// =============================================================================
#define MY_ID             ID_STICK1  // Change: ID_STICK1, ID_STICK2, ID_STICK3, ID_STICK4

// Host MAC - UPDATE THIS!
uint8_t hostMac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};  // TODO: ESP32-S3 MAC

// =============================================================================
// PINS (ESP-12F / NodeMCU)
// =============================================================================
#define PIN_BUTTON        14  // GPIO14 (D5)
#define PIN_MOTOR         12  // GPIO12 (D6) - PWM to transistor
#define PIN_SDA           4   // GPIO4 (D2)
#define PIN_SCL           5   // GPIO5 (D1)

// =============================================================================
// MPU-6050 REGISTERS
// =============================================================================
#define MPU_ADDR          0x68
#define MPU_PWR_MGMT_1    0x6B
#define MPU_ACCEL_XOUT_H  0x3B

// =============================================================================
// SHAKE DETECTION
// =============================================================================
#define SHAKE_THRESHOLD   15000
#define SHAKE_COOLDOWN    100

// =============================================================================
// JOYSTICK STATES
// =============================================================================
typedef enum {
  STATE_IDLE = 0,
  STATE_JOINING,
  STATE_COUNTDOWN,
  STATE_REACTION_WAIT,
  STATE_REACTION_ACTIVE,
  STATE_SHAKE_ACTIVE,
  STATE_FINISHED,
  STATE_PENALTY
} JoystickState;

// =============================================================================
// GLOBALS
// =============================================================================
volatile JoystickState currentState = STATE_IDLE;
volatile bool hostPaired = false;
volatile uint8_t assignedId = 0;

unsigned long gameStartTime = 0;
unsigned long vibrateEndTime = 0;
unsigned long lastShakeTime = 0;

uint8_t gameMode = 0;
uint8_t shakeTarget = 0;
uint8_t shakeCount = 0;
int32_t lastMagnitude = 0;

// =============================================================================
// SEND PACKET
// =============================================================================
void sendToHost(uint8_t cmd, uint16_t data) {
  if (!hostPaired) return;
  GamePacket pkt;
  buildPacket(&pkt, ID_HOST, assignedId ? assignedId : MY_ID, cmd, data);
  esp_now_send(hostMac, (uint8_t*)&pkt, sizeof(pkt));
}

// =============================================================================
// MOTOR CONTROL
// =============================================================================
void vibrate(uint16_t ms) {
  analogWrite(PIN_MOTOR, 255);
  vibrateEndTime = millis() + ms;
}

void updateMotor() {
  if (vibrateEndTime > 0 && millis() >= vibrateEndTime) {
    analogWrite(PIN_MOTOR, 0);
    vibrateEndTime = 0;
  }
}

// =============================================================================
// MPU-6050
// =============================================================================
bool mpuInit() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(MPU_PWR_MGMT_1);
  Wire.write(0);  // Wake up
  return Wire.endTransmission() == 0;
}

int32_t readAccelMagnitude() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(MPU_ACCEL_XOUT_H);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 6);
  
  int16_t ax = (Wire.read() << 8) | Wire.read();
  int16_t ay = (Wire.read() << 8) | Wire.read();
  int16_t az = (Wire.read() << 8) | Wire.read();
  
  return abs(ax) + abs(ay) + abs(az);  // Manhattan magnitude
}

bool detectShake() {
  if (millis() - lastShakeTime < SHAKE_COOLDOWN) return false;
  
  int32_t mag = readAccelMagnitude();
  int32_t delta = abs(mag - lastMagnitude);
  lastMagnitude = mag;
  
  if (delta > SHAKE_THRESHOLD) {
    lastShakeTime = millis();
    return true;
  }
  return false;
}

// =============================================================================
// ESP-NOW CALLBACK
// =============================================================================
void OnDataRecv(uint8_t* mac, uint8_t* data, uint8_t len) {
  if (len != sizeof(GamePacket)) return;
  
  GamePacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (!validatePacket(&pkt)) return;
  
  // Accept broadcast or packets addressed to us
  if (pkt.dest_id != ID_BROADCAST && pkt.dest_id != MY_ID && pkt.dest_id != assignedId) return;
  
  Serial.printf("[RX] cmd=0x%02X data=%d\n", pkt.cmd, packetData(&pkt));
  
  switch (pkt.cmd) {
    case CMD_OK:
      // Join confirmed
      assignedId = pkt.data_low;
      currentState = STATE_IDLE;
      vibrate(200);
      Serial.printf("Joined as ID %d\n", assignedId);
      break;
      
    case CMD_COUNTDOWN:
      currentState = STATE_COUNTDOWN;
      vibrate(150);
      break;
      
    case CMD_GAME_START:
      gameMode = pkt.data_high;
      shakeTarget = pkt.data_low;
      shakeCount = 0;
      if (gameMode == MODE_SHAKE) {
        currentState = STATE_SHAKE_ACTIVE;
        gameStartTime = millis();
      } else {
        currentState = STATE_REACTION_WAIT;
      }
      break;
      
    case CMD_VIBRATE:
      if (pkt.data_low == VIBRATE_GO) {
        // GO signal!
        if (currentState == STATE_REACTION_WAIT) {
          // Check for early press
          if (digitalRead(PIN_BUTTON) == HIGH) {
            currentState = STATE_PENALTY;
            sendToHost(CMD_REACTION_DONE, TIME_PENALTY);
            vibrate(100); delay(100); vibrate(100);
            Serial.println("PENALTY: Early press");
          } else {
            currentState = STATE_REACTION_ACTIVE;
            gameStartTime = millis();
            vibrate(300);
          }
        } else {
          vibrate(300);
        }
      } else {
        vibrate(pkt.data_low * 10);
      }
      break;
      
    case CMD_IDLE:
      currentState = STATE_IDLE;
      break;
  }
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  Serial.printf("\n=== Joystick %d ===\n", MY_ID);
  
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_MOTOR, OUTPUT);
  analogWrite(PIN_MOTOR, 0);
  
  Wire.begin(PIN_SDA, PIN_SCL);
  if (mpuInit()) Serial.println(F("MPU-6050 OK"));
  else Serial.println(F("MPU-6050 FAIL"));
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifi_set_channel(ESPNOW_CHANNEL);
  
  if (esp_now_init() != 0) {
    Serial.println(F("ESP-NOW fail"));
    return;
  }
  
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(OnDataRecv);
  esp_now_add_peer(hostMac, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, NULL, 0);
  
  hostPaired = true;
  Serial.print(F("MAC: ")); Serial.println(WiFi.macAddress());
  Serial.println(F("Ready - Press button to join"));
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
  updateMotor();
  
  bool buttonPressed = (digitalRead(PIN_BUTTON) == HIGH);
  
  switch (currentState) {
    case STATE_IDLE:
      // Press button to request join
      if (buttonPressed) {
        currentState = STATE_JOINING;
        sendToHost(CMD_REQ_ID, MY_ID);
        Serial.println(F("Requesting join..."));
        delay(500);  // Debounce
      }
      break;
      
    case STATE_JOINING:
      // Waiting for CMD_OK
      break;
      
    case STATE_COUNTDOWN:
      // Waiting for game start
      break;
      
    case STATE_REACTION_WAIT:
      // Waiting for GO signal (CMD_VIBRATE 0xFF)
      // Early press is checked in OnDataRecv
      break;
      
    case STATE_REACTION_ACTIVE:
      if (buttonPressed) {
        uint16_t reactionTime = (uint16_t)(millis() - gameStartTime);
        currentState = STATE_FINISHED;
        sendToHost(CMD_REACTION_DONE, reactionTime);
        Serial.printf("Reaction: %dms\n", reactionTime);
        delay(100);
      }
      break;
      
    case STATE_SHAKE_ACTIVE:
      if (detectShake()) {
        shakeCount++;
        vibrate(50);
        Serial.printf("Shake: %d/%d\n", shakeCount, shakeTarget);
        
        if (shakeCount >= shakeTarget) {
          uint16_t shakeTime = (uint16_t)(millis() - gameStartTime);
          currentState = STATE_FINISHED;
          sendToHost(CMD_SHAKE_DONE, shakeTime);
          Serial.printf("Shake done: %dms\n", shakeTime);
        }
      }
      break;
      
    case STATE_FINISHED:
    case STATE_PENALTY:
      // Waiting for CMD_IDLE
      break;
  }
  
  delay(1);
}
