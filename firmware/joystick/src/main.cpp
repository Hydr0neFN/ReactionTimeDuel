/*
 * Reaction Time Duel - ESP8266 joystick firmware 2.0
 *
 * Hardware: ESP-12F on the custom PCB
 *   GPIO14: button (active LOW, pullup)
 *   GPIO12: vibration motor
 *   GPIO4:  MPU-6050 SDA
 *   GPIO5:  MPU-6050 SCL
 *
 * This file only implements the hardware ports of StickLogic (DESIGN.md 6).
 * All game logic lives in shared/rtd_core. The radio callbacks and the button
 * ISR only copy data into fixed-size ring buffers; the main loop runs the logic.
 */

#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266httpUpdate.h>
#include <Wire.h>
#include <espnow.h>
#include <new>

#include "RtdDiag.h"
#include "RtdNetConfig.h"
#include "RtdProtocol.h"
#include "StickLogic.h"

// =============================================================================
// PIN DEFINITIONS (from schematic)
// =============================================================================
#define PIN_BUTTON        14    // GPIO14 - SW4 button (active LOW, pullup)
#define PIN_MOTOR         12    // GPIO12 - vibration motor
#define PIN_SDA           4     // GPIO4  - MPU-6050 SDA
#define PIN_SCL           5     // GPIO5  - MPU-6050 SCL

// =============================================================================
// MPU-6050
// =============================================================================
#define MPU_ADDR          0x68
#define MPU_REG_PWR_MGMT1 0x6B
#define MPU_REG_ACCEL_XH  0x3B

// =============================================================================
// ID STORAGE (DESIGN.md J5: EEPROM record -> MAC table -> none)
// =============================================================================
#define ID_EEPROM_SIZE    16
#define ID_EEPROM_MAGIC   0xA5

typedef struct {
  uint8_t magic;
  uint8_t id;
} EeIdRecord;

// =============================================================================
// OTA (DESIGN.md 9.3)
// =============================================================================
#define OTA_URL                       "http://" RTD_HOST_IP "/fw/joystick.bin"
#define OTA_JOIN_TIMEOUT_MS           20000UL
#define OTA_RECOVERY_JOIN_TIMEOUT_MS  60000UL
#define OTA_RECOVERY_HOLD_MS          5000UL

// =============================================================================
// RING BUFFERS (fixed size, no heap, filled by the callbacks / ISR)
// =============================================================================
#define RX_RING_SIZE      8
#define BTN_RING_SIZE     16

typedef struct {
  uint8_t  data[PACKET_SIZE];
  uint8_t  len;
  uint8_t  mac[6];
  uint32_t rxMicros;
} RxEntry;

static RxEntry rxRing[RX_RING_SIZE];
static volatile uint8_t rxHead = 0;   // written by the ESP-NOW callback
static volatile uint8_t rxTail = 0;   // written by the loop

static uint32_t btnRing[BTN_RING_SIZE];
static volatile uint8_t btnHead = 0;  // written by the ISR
static volatile uint8_t btnTail = 0;  // written by the loop

// =============================================================================
// ADDRESS LEARNING (DESIGN.md 3.2)
// =============================================================================
static uint8_t hostMac[6] = RTD_MAC_HOST;

// =============================================================================
// PORTS
// =============================================================================
class EspStickPorts : public StickPorts {
 public:
  EspStickPorts() : joinTimeoutMs_(OTA_JOIN_TIMEOUT_MS) {}

  void setJoinTimeout(uint32_t ms) { joinTimeoutMs_ = ms; }

  uint32_t millis() override { return (uint32_t)::millis(); }

  uint32_t micros() override { return (uint32_t)::micros(); }

  void send(const uint8_t* data, uint8_t len) override {
    esp_now_send(hostMac, (uint8_t*)data, len);
  }

  void motor(bool on) override { digitalWrite(PIN_MOTOR, on ? HIGH : LOW); }

  bool buttonDown() override { return digitalRead(PIN_BUTTON) == LOW; }

  bool readAccel(int16_t& ax, int16_t& ay, int16_t& az) override {
    uint8_t data[6];
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(MPU_REG_ACCEL_XH);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)MPU_ADDR, 6) != 6) return false;
    for (uint8_t i = 0; i < 6; i++) data[i] = (uint8_t)Wire.read();
    ax = (int16_t)((data[0] << 8) | data[1]);
    ay = (int16_t)((data[2] << 8) | data[3]);
    az = (int16_t)((data[4] << 8) | data[5]);
    return true;
  }

  void otaStart(uint8_t verByte, uint8_t patch) override;

 private:
  uint32_t joinTimeoutMs_;
};

EspStickPorts ports;

// =============================================================================
// LOGIC INSTANCE (created in setup once the stick ID is known)
// =============================================================================
static uint8_t logicStorage[sizeof(StickLogic)] __attribute__((aligned(4)));
static StickLogic* logic = nullptr;
static uint8_t stickId = 0;

// =============================================================================
// DIAGNOSTICS (DESIGN.md 11.1) - counters only in the callbacks and the ISR
// =============================================================================
static volatile uint32_t diagRxDrop = 0;    // written by the ESP-NOW callback
static volatile uint32_t diagTxFail = 0;    // written by the send callback
static volatile uint32_t diagBtnEdges = 0;  // written by the ISR
static volatile uint32_t diagBtnDrop = 0;   // written by the ISR
static uint32_t diagBadFrame = 0;           // written by the loop
static uint32_t diagLoopMaxMs = 0;          // since the last report of its key
static uint32_t diagLinkGapDs = 0;          // since the last report of its key
static bool mpuOk = false;

// =============================================================================
// ESP-NOW RECEIVE CALLBACK - copies the frame and its timestamp into the ring
// =============================================================================
static void IRAM_ATTR onEspNowRecv(uint8_t* mac, uint8_t* data, uint8_t len) {
  if (len != (uint8_t)PACKET_SIZE) return;
  uint8_t next = (uint8_t)((rxHead + 1) % RX_RING_SIZE);
  if (next == rxTail) {                   // ring full - drop
    diagRxDrop++;
    return;
  }
  RxEntry& entry = rxRing[rxHead];
  memcpy(entry.data, data, PACKET_SIZE);
  entry.len = len;
  memcpy(entry.mac, mac, 6);
  entry.rxMicros = ::micros();
  rxHead = next;
}

static void onEspNowSent(uint8_t* mac, uint8_t status) {
  (void)mac;
  if (status != 0) diagTxFail++;
}

// =============================================================================
// BUTTON ISR - stores the micros() of every falling edge
// =============================================================================
static void IRAM_ATTR onButtonFallIsr() {
  uint8_t next = (uint8_t)((btnHead + 1) % BTN_RING_SIZE);
  if (next == btnTail) {                  // ring full - drop
    diagBtnDrop++;
    return;
  }
  btnRing[btnHead] = ::micros();
  btnHead = next;
  diagBtnEdges++;
}

// =============================================================================
// ID FROM THE MAC TABLE (RtdNetConfig.h), only for the OTA image without MY_ID
// =============================================================================
#ifndef MY_ID
static uint8_t idFromMacTable() {
  static const uint8_t table[RTD_MAX_PLAYERS][6] = {
    RTD_MAC_STICK1, RTD_MAC_STICK2, RTD_MAC_STICK3, RTD_MAC_STICK4
  };
  uint8_t mac[6];
  WiFi.macAddress(mac);
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (memcmp(mac, table[i], 6) == 0) return (uint8_t)(ID_STICK1 + i);
  }
  return 0;
}
#endif

static uint8_t readIdFromEeprom() {
  EeIdRecord rec = {0, 0};
  EEPROM.get(0, rec);
  if (rec.magic != ID_EEPROM_MAGIC) return 0;
  if (rec.id < ID_STICK1 || rec.id > ID_STICK4) return 0;
  return rec.id;
}

#ifdef MY_ID
static void writeIdToEeprom(uint8_t id) {
  EeIdRecord rec;
  rec.magic = ID_EEPROM_MAGIC;
  rec.id = id;
  EEPROM.put(0, rec);
  EEPROM.commit();
}
#endif

// =============================================================================
// OTA (DESIGN.md 9.3)
// =============================================================================
static bool radioStarted = false;

static void otaReboot() {
  delay(100);
  ESP.restart();
  while (true) {
    delay(10);
  }
}

static bool otaJoinAp(uint32_t timeoutMs) {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.begin(RTD_AP_SSID, RTD_AP_PASS);
  uint32_t start = ::millis();
  while (WiFi.status() != WL_CONNECTED) {
    if ((uint32_t)(::millis() - start) >= timeoutMs) return false;
    delay(100);
  }
  Serial.printf("[OTA] joined %s, ip %s\n", RTD_AP_SSID, WiFi.localIP().toString().c_str());
  return true;
}

void EspStickPorts::otaStart(uint8_t verByte, uint8_t patch) {
  Serial.printf("[OTA] starting %s (target %d.%d)\n", OTA_URL, verByte >> 4, verByte & 0x0F);
  (void)patch;

  digitalWrite(PIN_MOTOR, LOW);
  delay(50);                              // let the ACK and status frames leave

  if (radioStarted) {
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    radioStarted = false;
  }

  if (!otaJoinAp(joinTimeoutMs_)) {
    Serial.println("[OTA] AP join failed - rebooting");
    otaReboot();
  }

  WiFiClient client;
  ESPhttpUpdate.rebootOnUpdate(false);
  t_httpUpdate_return result = ESPhttpUpdate.update(client, OTA_URL);
  Serial.printf("[OTA] update result %d - rebooting\n", (int)result);

  otaReboot();
}

// =============================================================================
// AP + ESP-NOW
// =============================================================================
static void addHostPeer() {
  esp_now_add_peer(hostMac, ESP_NOW_ROLE_COMBO, ESPNOW_CHANNEL, NULL, 0);
}

// Replace the host peer when a valid frame from another MAC arrives (3.2).
static void learnHostMac(const RxEntry& entry) {
  if (!validatePacket(entry.data, entry.len)) return;
  if (entry.data[2] != ID_HOST) return;   // src_id
  if (memcmp(entry.mac, hostMac, 6) == 0) return;

  Serial.printf("[NET] host learned %02X:%02X:%02X:%02X:%02X:%02X\n",
                entry.mac[0], entry.mac[1], entry.mac[2],
                entry.mac[3], entry.mac[4], entry.mac[5]);
  esp_now_del_peer(hostMac);
  memcpy(hostMac, entry.mac, 6);
  addHostPeer();
}

static bool startRadio() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setOutputPower(20.5);              // maximum TX power
  wifi_set_channel(ESPNOW_CHANNEL);

  if (esp_now_init() != 0) {
    Serial.println("[NET] ESP-NOW init failed");
    return false;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onEspNowRecv);
  esp_now_register_send_cb(onEspNowSent);
  addHostPeer();
  radioStarted = true;

  Serial.printf("[NET] MAC %s, id %u, host %02X:%02X:%02X:%02X:%02X:%02X\n",
                WiFi.macAddress().c_str(), stickId,
                hostMac[0], hostMac[1], hostMac[2], hostMac[3], hostMac[4], hostMac[5]);
  return true;
}

// =============================================================================
// NO-ID MODE: three motor pulses every 2 s, nothing else (DESIGN.md J5)
// =============================================================================
static void runNoIdPulse() {
  uint32_t elapsed = (uint32_t)(::millis() % 2000UL);
  bool on = elapsed < 600UL && (elapsed % 200UL) < 100UL;
  digitalWrite(PIN_MOTOR, on ? HIGH : LOW);
  delay(1);
}

// =============================================================================
// MPU-6050
// =============================================================================
static void mpuWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static void mpuInit() {
  mpuWriteReg(MPU_REG_PWR_MGMT1, 0x00);   // wake up
  delay(10);
}

static bool mpuProbe() {
  Wire.beginTransmission(MPU_ADDR);
  return Wire.endTransmission() == 0;
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n========================================");
  Serial.println("     REACTION TIME DUEL - JOYSTICK");
  Serial.println("            Firmware " FW_VERSION_STRING);
  Serial.println("========================================");
  Serial.println(RTD_FW_MARKER(joystick));
  Serial.printf("[BOOT] reset reason %d\n", (int)ESP.getResetInfoPtr()->reason);

  pinMode(PIN_MOTOR, OUTPUT);
  digitalWrite(PIN_MOTOR, LOW);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  EEPROM.begin(ID_EEPROM_SIZE);

  // Recovery: button held 5 s at power-on goes straight to the OTA download.
  if (digitalRead(PIN_BUTTON) == LOW) {
    uint32_t start = ::millis();
    while (digitalRead(PIN_BUTTON) == LOW &&
           (uint32_t)(::millis() - start) < OTA_RECOVERY_HOLD_MS) {
      delay(10);
    }
    if (digitalRead(PIN_BUTTON) == LOW) {
      Serial.println("[OTA] recovery mode - button held at power-on");
      ports.setJoinTimeout(OTA_RECOVERY_JOIN_TIMEOUT_MS);
      ports.otaStart(FW_VERSION_BYTE, FW_VERSION_PATCH);
    }
  }

  // Stick ID: build flag -> EEPROM record -> MAC table -> none.
  uint8_t eeId = readIdFromEeprom();
#ifdef MY_ID
  stickId = (uint8_t)(MY_ID);
  if (eeId != stickId) {
    writeIdToEeprom(stickId);
    Serial.printf("[ID] EEPROM record written: %u\n", stickId);
  }
#else
  stickId = eeId;
  if (stickId == 0) stickId = idFromMacTable();
  Serial.printf("[ID] EEPROM %u, selected %u\n", eeId, stickId);
#endif

  if (stickId == 0) {
    Serial.println("[ID] no valid stick ID - motor pulses only");
    return;
  }

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  mpuInit();
  mpuOk = mpuProbe();
  Serial.println("MPU-6050 ready");

  if (!startRadio()) return;

  attachInterrupt(digitalPinToInterrupt(PIN_BUTTON), onButtonFallIsr, FALLING);

  logic = new (logicStorage) StickLogic(ports, stickId);
  Serial.println("Joystick ready");
}

// =============================================================================
// DIAGNOSTICS (DESIGN.md 11.1) - one DIAG_* key per DIAG_INTERVAL_MS, JS_IDLE only
// =============================================================================
static const uint8_t DIAG_STICK_KEYS[] = {
  DIAG_FW_VERSION, DIAG_MPU_OK, DIAG_STICK_STATE,   // the keys of the STICKS page come first
  DIAG_RESET_REASON, DIAG_UPTIME_MIN, DIAG_HEAP_KB,
  DIAG_RX_DROP, DIAG_TX_FAIL, DIAG_BAD_FRAME, DIAG_LOOP_MAX_MS, DIAG_LINK_GAP_DS,
  DIAG_BTN_EDGES, DIAG_BTN_DROP
};
#define DIAG_STICK_KEY_COUNT  (sizeof(DIAG_STICK_KEYS) / sizeof(DIAG_STICK_KEYS[0]))

static uint32_t diagValue(uint8_t key) {
  switch (key) {
    case DIAG_FW_VERSION:   return FW_VERSION_BYTE;
    case DIAG_RESET_REASON: return (uint32_t)ESP.getResetInfoPtr()->reason;
    case DIAG_UPTIME_MIN:   return ::millis() / 60000UL;
    case DIAG_HEAP_KB:      return ESP.getFreeHeap() / 1024UL;
    case DIAG_RX_DROP:      return diagRxDrop;
    case DIAG_TX_FAIL:      return diagTxFail;
    case DIAG_BAD_FRAME:    return diagBadFrame;
    case DIAG_LOOP_MAX_MS:  return diagLoopMaxMs;
    case DIAG_LINK_GAP_DS:  return diagLinkGapDs;
    case DIAG_BTN_EDGES:    return diagBtnEdges;
    case DIAG_BTN_DROP:     return diagBtnDrop;
    case DIAG_STICK_STATE:  return logic->state();
    case DIAG_MPU_OK:       return mpuOk ? 1 : 0;
    default:                return 0;
  }
}

static void diagTick(uint32_t now) {
  static uint8_t diagKeyIdx = 0;
  static uint32_t diagNextMs = 0;

  if (!radioStarted || stickId == 0 || logic->state() != JS_IDLE) return;  // also no send in OTA
  if ((int32_t)(now - diagNextMs) < 0) return;
  // service menu open (host in PH_MAINT): a full rotation takes 2.6 s and not 13 s
  diagNextMs = now + (logic->hostPhase() == PH_MAINT ? DIAG_INTERVAL_FAST_MS : DIAG_INTERVAL_MS);

  uint8_t key = DIAG_STICK_KEYS[diagKeyIdx];
  diagKeyIdx = (uint8_t)((diagKeyIdx + 1) % DIAG_STICK_KEY_COUNT);
  uint32_t value = diagValue(key);
  if (key == DIAG_LOOP_MAX_MS) diagLoopMaxMs = 0;   // max values reset after their report
  if (key == DIAG_LINK_GAP_DS) diagLinkGapDs = 0;

  GamePacket pkt;
  buildPacket(&pkt, ID_HOST, stickId, CMD_DIAG,
              (uint16_t)(((uint16_t)key << 8) | rtdDiagClamp(value)));
  ports.send((const uint8_t*)&pkt, sizeof(pkt));
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
  uint32_t loopStartMs = ::millis();

  if (logic == nullptr) {
    if (stickId == 0) {
      runNoIdPulse();
    } else {
      delay(1);
    }
    return;
  }

  while (rxTail != rxHead) {
    RxEntry entry;
    memcpy(&entry, &rxRing[rxTail], sizeof(entry));
    rxTail = (uint8_t)((rxTail + 1) % RX_RING_SIZE);
    if (!validatePacket(entry.data, entry.len)) {
      diagBadFrame++;
    } else if (entry.data[2] == ID_HOST) {
      // Host link gap: a valid host frame restarts the measurement.
      static uint32_t lastHostMs = 0;
      if (lastHostMs != 0) {
        uint32_t gapDs = (uint32_t)(::millis() - lastHostMs) / 100UL;
        if (gapDs > diagLinkGapDs) diagLinkGapDs = gapDs;
      }
      lastHostMs = ::millis();
    }
    learnHostMac(entry);
    logic->onPacket(entry.data, entry.len, entry.rxMicros);
  }

  while (btnTail != btnHead) {
    uint32_t us = btnRing[btnTail];
    btnTail = (uint8_t)((btnTail + 1) % BTN_RING_SIZE);
    logic->onButtonFall(us);
  }

  logic->tick();

  static uint8_t lastState = 0xFF;
  if (logic->state() != lastState) {
    lastState = logic->state();
    Serial.printf("[STICK] state %u slot %u\n", lastState, logic->assignedSlot());
  }

  delay(1);

  uint32_t loopMs = (uint32_t)(::millis() - loopStartMs);
  if (loopMs > diagLoopMaxMs) diagLoopMaxMs = loopMs;

  diagTick(::millis());
}
