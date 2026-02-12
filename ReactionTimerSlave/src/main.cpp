/*
 * joystick_test.cpp - ESP8266 Joystick - Full Game Firmware
 *
 * Hardware: ESP-12F on custom PCB
 * MAC Joystick 1: BC:FF:4D:F9:F3:91
 * MAC Joystick 2: BC:FF:4D:F9:AE:29
 * MAC Joystick 3: (fill in)
 * MAC Joystick 4: (fill in)
 *
 * Pins:
 *   GPIO14: Button (SW4) - active LOW with pullup
 *   GPIO12: Vibration motor output
 *   GPIO4:  SDA (MPU-6050)
 *   GPIO5:  SCL (MPU-6050)
 *
 * Game modes:
 *   REACTION: Wait for ESP-NOW GO -> start timer -> wait for button press -> send time
 *   SHAKE:    Wait for ESP-NOW GO -> count shakes via MPU-6050 -> send time when target reached
 *
 * Timing:
 *   The CMD_GO ESP-NOW message starts the timer.
 *   Button press (GPIO14 falling edge) stops the timer via IRAM interrupt (reaction mode).
 *   Shake completion time is recorded when shake count reaches target.
 *
 * Communication:
 *   ESP-NOW to Host ESP32 (unicast)
 *   CMD_GAME_START received -> sets mode + param
 *   CMD_GO received -> start timing + vibrate motor (haptic GO cue)
 *   CMD_COUNTDOWN -> vibrate briefly (haptic countdown cue)
 *   CMD_REACTION_DONE / CMD_SHAKE_DONE sent back with time_ms
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <Wire.h>
#include "Protocol.h"
#include "GameTypes.h"

// =============================================================================
// CONFIGURATION - MY_ID set via platformio.ini build flag
// =============================================================================
#ifndef MY_ID
#define MY_ID             ID_STICK1
#endif

// =============================================================================
// PIN DEFINITIONS (from schematic)
// =============================================================================
#define PIN_BUTTON        14    // GPIO14 - SW4 button (active LOW, pullup)
#define PIN_MOTOR         12    // GPIO12 - vibration motor
#define PIN_SDA           4     // GPIO4  - MPU-6050 SDA
#define PIN_SCL           5     // GPIO5  - MPU-6050 SCL

// =============================================================================
// MPU-6050 CONSTANTS
// =============================================================================
#define MPU_ADDR          0x68
#define MPU_REG_PWR_MGMT1 0x6B
#define MPU_REG_ACCEL_XH  0x3B

// Shake detection tuning (X+Z axes only, high-pass filtered to remove gravity)
// A shake = full cycle: dynamic energy exceeds HIGH, then returns below LOW
#define SHAKE_THRESHOLD_HIGH  6000   // dynamic XZ energy to detect peak (push)
#define SHAKE_THRESHOLD_LOW   2000   // dynamic XZ energy to detect return (pull-back)

// =============================================================================
// ESP-NOW
// =============================================================================
uint8_t hostMac[6] = {0x88, 0x57, 0x21, 0xB3, 0x05, 0xAC};

// =============================================================================
// GAME STATE
// =============================================================================
enum JoystickState : uint8_t {
  JS_IDLE,
  JS_WAITING_GO,        // received GAME_START, waiting for ESP-NOW CMD_GO
  JS_REACTION_TIMING,   // GO received, waiting for button press
  JS_SHAKE_COUNTING,    // GO received, counting shakes
  JS_DONE               // result sent, waiting for next round
};

JoystickState jsState = JS_IDLE;
uint8_t currentMode = MODE_REACTION;
uint8_t shakeTarget = 10;
uint8_t assignedSlot = 0;           // which player slot we were assigned (1-4), 0 = not assigned

// Button debouncing for join (capacitor causes slow edges)
bool lastButtonState = HIGH;
unsigned long lastButtonChange = 0;
bool joinSent = false;              // prevent repeat join sends while held
#define DEBOUNCE_MS 50

// =============================================================================
// PRECISE TIMING
// =============================================================================
volatile uint32_t g_go_time_us = 0;       // timestamp when GO received
volatile bool g_go_received = false;       // flag: GO happened
volatile uint32_t g_button_time_us = 0;   // timestamp when button pressed
volatile bool g_button_pressed = false;    // flag: button was pressed

// Button press: falling edge on GPIO14 (active LOW)
void IRAM_ATTR onButton() {
  if (g_go_received && jsState == JS_REACTION_TIMING) {
    g_button_time_us = micros();
    g_button_pressed = true;
  }
}

// Called when CMD_GO is received via ESP-NOW
void handleGO() {
  g_go_time_us = micros();
  g_go_received = true;
}

// =============================================================================
// MPU-6050 FUNCTIONS
// =============================================================================
void mpuWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

bool mpuReadBlock(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

bool mpuInit() {
  mpuWriteReg(MPU_REG_PWR_MGMT1, 0x00); // wake up
  delay(10);  // reduced from 100ms - MPU-6050 wakes faster
  return true;
}

bool mpuReadAccel(int16_t &ax, int16_t &ay, int16_t &az) {
  uint8_t data[6];
  if (!mpuReadBlock(MPU_REG_ACCEL_XH, data, 6)) return false;
  ax = (int16_t)((data[0] << 8) | data[1]);
  ay = (int16_t)((data[2] << 8) | data[3]);
  az = (int16_t)((data[4] << 8) | data[5]);
  return true;
}

// =============================================================================
// MOTOR VIBRATION
// =============================================================================
unsigned long vibEndTime = 0;
bool vibActive = false;

void vibStart(uint16_t duration_ms) {
  digitalWrite(PIN_MOTOR, HIGH);
  vibEndTime = millis() + duration_ms;
  vibActive = true;
}

void vibUpdate() {
  if (vibActive && millis() >= vibEndTime) {
    digitalWrite(PIN_MOTOR, LOW);
    vibActive = false;
  }
}

// =============================================================================
// ESP-NOW SEND
// =============================================================================
void sendToHost(uint8_t cmd, uint16_t data) {
  GamePacket pkt;
  buildPacket(&pkt, ID_HOST, MY_ID, cmd, data);
  int result = esp_now_send(hostMac, (uint8_t*)&pkt, sizeof(pkt));
  Serial.printf("[SEND] cmd=0x%02X data=%d result=%d\n", cmd, data, result);
}

// =============================================================================
// ESP-NOW CALLBACK
// =============================================================================
void OnDataRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len != (uint8_t)sizeof(GamePacket)) return;

  GamePacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (!validatePacket(&pkt)) return;
  if (pkt.dest_id != MY_ID && pkt.dest_id != ID_BROADCAST) return;

  switch (pkt.cmd) {
    case CMD_IDLE:
      jsState = JS_IDLE;
      assignedSlot = 0;  // reset slot assignment
      joinSent = false;  // allow new join request
      g_go_received = false;
      g_button_pressed = false;
      Serial.println("[CMD] IDLE");
      break;

    case CMD_OK:
      // Host acknowledged our join request, data = slot number (1-4)
      assignedSlot = pkt.data_low;
      vibStart(200);  // haptic feedback for successful join
      Serial.printf("[CMD] OK - assigned to Player %d slot\n", assignedSlot);
      break;

    case CMD_GAME_START:
      // data_high = mode, data_low = param (shake target or 0)
      currentMode = pkt.data_high;
      shakeTarget = pkt.data_low;
      jsState = JS_WAITING_GO;
      g_go_received = false;
      g_button_pressed = false;
      Serial.printf("[CMD] GAME_START mode=%d param=%d\n", currentMode, shakeTarget);
      break;

    case CMD_VIBRATE:
      // Short vibrate (duration x 10ms)
      vibStart(pkt.data_low * 10);
      Serial.printf("[CMD] VIBRATE %d\n", pkt.data_low);
      break;

    case CMD_COUNTDOWN:
      // Haptic countdown pulse: 200ms vibrate
      vibStart(200);
      Serial.printf("[CMD] COUNTDOWN %d\n", pkt.data_low);
      break;

    case CMD_GO:
      // GO signal received via ESP-NOW - start timing!
      if (jsState == JS_WAITING_GO) {
        handleGO();  // sets g_go_time_us and g_go_received
        Serial.println("[CMD] GO received!");
      }
      break;

    default:
      break;
  }
}

void OnDataSent(uint8_t *mac, uint8_t status) {
  // optional
}

// =============================================================================
// SHAKE COUNTING (non-blocking, called in loop during JS_SHAKE_COUNTING)
// =============================================================================
uint16_t shakeCount = 0;
bool shakePeaked = false;           // true = saw peak, waiting for return
uint32_t shakeStartTime_ms = 0;

// High-pass filter state: Q8 fixed-point low-pass filter on X and Z
// Subtracting the low-pass (gravity) from raw readings gives the dynamic (shake) component
int32_t shakeLpfAx = 0;
int32_t shakeLpfAz = 0;
bool shakeLpfReady = false;

void shakeReset() {
  shakeCount = 0;
  shakePeaked = false;
  shakeLpfReady = false;
  shakeStartTime_ms = millis();
}

// Returns: 0 = still counting, TIME_PENALTY = timeout, >0 = completion time in ms
uint16_t shakeUpdate() {
  int16_t ax, ay, az;
  if (!mpuReadAccel(ax, ay, az)) return 0;

  if (!shakeLpfReady) {
    // Seed the low-pass filter with current reading (Q8 fixed-point)
    shakeLpfAx = (int32_t)ax << 8;
    shakeLpfAz = (int32_t)az << 8;
    shakeLpfReady = true;
    return 0;
  }

  // Update low-pass filter: EMA with alpha = 1/64 (tracks slow gravity changes only)
  shakeLpfAx += (((int32_t)ax << 8) - shakeLpfAx) >> 6;
  shakeLpfAz += (((int32_t)az << 8) - shakeLpfAz) >> 6;

  // High-pass = raw - low-pass → removes gravity, keeps dynamic shake energy
  int32_t dynamicX = (int32_t)ax - (shakeLpfAx >> 8);
  int32_t dynamicZ = (int32_t)az - (shakeLpfAz >> 8);
  int32_t energy = abs(dynamicX) + abs(dynamicZ);

  // Hysteresis state machine: peak (push) → return (pull-back) = one shake
  if (!shakePeaked) {
    if (energy > SHAKE_THRESHOLD_HIGH) {
      shakePeaked = true;
    }
  } else {
    if (energy < SHAKE_THRESHOLD_LOW) {
      shakeCount++;
      shakePeaked = false;
      Serial.printf("[SHAKE] count=%d/%d  energy=%ld\n", shakeCount, shakeTarget, (long)energy);
    }
  }

  // Check if target reached
  if (shakeCount >= shakeTarget) {
    uint32_t elapsed = millis() - shakeStartTime_ms;
    // Cap at 0xFFFE (0xFFFF is penalty)
    if (elapsed > 0xFFFE) elapsed = 0xFFFE;
    return (uint16_t)elapsed;
  }

  // Timeout (30s)
  if (millis() - shakeStartTime_ms > TIMEOUT_SHAKE) {
    return TIME_PENALTY;
  }

  return 0; // still going
}

// =============================================================================
// MAIN STATE MACHINE (runs in loop)
// =============================================================================
void runJoystick() {
  vibUpdate(); // keep motor timing working

  switch (jsState) {
    case JS_IDLE: {
      // Poll button for join request (debounced due to capacitor)
      bool currentButton = digitalRead(PIN_BUTTON);
      unsigned long now = millis();

      // Track when button state changes
      if (currentButton != lastButtonState) {
        lastButtonChange = now;
        lastButtonState = currentButton;
      }

      // Act only after button held LOW for DEBOUNCE_MS (stable press)
      if (currentButton == LOW && (now - lastButtonChange) > DEBOUNCE_MS && !joinSent) {
        // Confirmed press - send join request if not already assigned
        if (assignedSlot == 0) {
          sendToHost(CMD_REQ_ID, 0);
          Serial.println("[JOIN] Button pressed - sending CMD_REQ_ID");
        } else {
          Serial.printf("[JOIN] Already assigned to slot %d\n", assignedSlot);
        }
        joinSent = true;  // prevent repeat sends while held
      }

      // Reset when button released
      if (currentButton == HIGH) {
        joinSent = false;
      }
      break;
    }

    case JS_WAITING_GO:
      // Wait for ESP-NOW CMD_GO (handleGO sets g_go_received)
      if (g_go_received) {
        Serial.println("[GO] ESP-NOW GO received!");

        // Haptic GO cue
        vibStart(500);

        if (currentMode == MODE_REACTION) {
          // Check for early button press (cheating)
          // Button is active LOW: if it reads LOW right now, they pressed early
          if (digitalRead(PIN_BUTTON) == LOW) {
            Serial.println("[REACTION] PENALTY - early press!");
            sendToHost(CMD_REACTION_DONE, TIME_PENALTY);
            jsState = JS_DONE;
          } else {
            jsState = JS_REACTION_TIMING;
            Serial.println("[REACTION] Waiting for button...");
          }
        } else if (currentMode == MODE_SHAKE) {
          shakeReset();
          jsState = JS_SHAKE_COUNTING;
          Serial.printf("[SHAKE] Counting to %d...\n", shakeTarget);
        }
      }
      break;

    case JS_REACTION_TIMING:
      // Wait for button press (interrupt sets g_button_pressed)
      if (g_button_pressed) {
        // Calculate time in ms from GO to button
        uint32_t elapsed_us = g_button_time_us - g_go_time_us;
        uint16_t elapsed_ms = (uint16_t)(elapsed_us / 1000);
        if (elapsed_ms == 0) elapsed_ms = 1; // minimum 1ms

        Serial.printf("[REACTION] Time: %d ms (%d us)\n", elapsed_ms, elapsed_us);
        sendToHost(CMD_REACTION_DONE, elapsed_ms);

        // Brief vibrate on completion
        vibStart(100);
        jsState = JS_DONE;
      }

      // Timeout (10s) - no button press
      // Use millis comparison against GO time (convert g_go_time_us to ms)
      if (millis() > (g_go_time_us / 1000) + TIMEOUT_REACTION) {
        Serial.println("[REACTION] TIMEOUT");
        sendToHost(CMD_REACTION_DONE, TIME_PENALTY);
        jsState = JS_DONE;
      }
      break;

    case JS_SHAKE_COUNTING: {
      uint16_t result = shakeUpdate();
      if (result > 0) {
        if (result == TIME_PENALTY) {
          Serial.println("[SHAKE] TIMEOUT");
        } else {
          Serial.printf("[SHAKE] Done! Time: %d ms\n", result);
          // Vibrate on completion
          vibStart(200);
        }
        sendToHost(CMD_SHAKE_DONE, result);
        jsState = JS_DONE;
      }
      break;
    }

    case JS_DONE:
      // Wait for next GAME_START or IDLE from host
      break;
  }
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== REACTION REIMAGINED - JOYSTICK ===");
  Serial.printf("My ID: 0x%02X\n", MY_ID);

  // Motor output
  pinMode(PIN_MOTOR, OUTPUT);
  digitalWrite(PIN_MOTOR, LOW);

  // Button input (active LOW)
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  // Button interrupt
  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), onButton, FALLING);

  // I2C + MPU-6050 (400kHz for faster reads)
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  if (mpuInit()) {
    Serial.println("MPU-6050 ready");
  } else {
    Serial.println("MPU-6050 init failed!");
  }

  // ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifi_set_channel(ESPNOW_CHANNEL);

  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW init failed!");
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(OnDataRecv);
  esp_now_register_send_cb(OnDataSent);

  // Pair with host
  int result = esp_now_add_peer(hostMac, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, NULL, 0);
  if (result == 0) {
    Serial.println("Host paired");
  } else {
    Serial.printf("Host pair failed: %d\n", result);
  }

  Serial.print("My MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println("Joystick ready!");
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
  runJoystick();
  yield();  // allow WiFi/system tasks without blocking - much faster than delay(5)
}
