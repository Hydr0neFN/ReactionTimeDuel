#include "Debug.h"

#include <Arduino.h>
#include <esp_system.h>
#include <stdio.h>
#include <string.h>

#include "Radio.h"

Debug debug;

/* The DIAG_* keys of RtdDiag.h, in report order; unknown keys get a free slot. */
static const uint8_t DIAG_KEY_LIST[] = {
    DIAG_FW_VERSION,  DIAG_RESET_REASON, DIAG_UPTIME_MIN,  DIAG_HEAP_KB,
    DIAG_RX_DROP,     DIAG_TX_FAIL,      DIAG_BAD_FRAME,   DIAG_LOOP_MAX_MS,
    DIAG_LINK_GAP_DS, DIAG_BTN_EDGES,    DIAG_BTN_DROP,    DIAG_STICK_STATE,
    DIAG_MPU_OK,      DIAG_SNAP_PER_10S, DIAG_TOUCH_OK,    DIAG_OTA_STATE,
    DIAG_SCREEN};
#define DIAG_KEY_COUNT ((uint8_t)sizeof(DIAG_KEY_LIST))

static const char* DEVICE_LABELS[DBG_DEVICE_COUNT] = {"STICK1", "STICK2", "STICK3",
                                                      "STICK4", "DISPLAY"};

/* Frames that are only logged in trace mode (DESIGN.md 11.2, 12). */
static bool quietCmd(uint8_t cmd) {
  return cmd == CMD_BEACON || cmd == CMD_HEARTBEAT || cmd == CMD_DIAG ||
         cmd == CMD_SHAKE_PROGRESS || cmd == CMD_SVC_REQ;
}

static const char* resetText(int reason) {
  switch ((esp_reset_reason_t)reason) {
    case ESP_RST_UNKNOWN:   return "UNKNOWN";
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "?";
  }
}

/* "cmd=GO" or "cmd=0x46" for an unknown command. */
static void cmdText(char* buf, size_t len, uint8_t cmd) {
  const char* name = Debug::cmdName(cmd);
  if (name != nullptr) {
    snprintf(buf, len, "cmd=%s", name);
  } else {
    snprintf(buf, len, "cmd=0x%02X", cmd);
  }
}

Debug::Debug()
    : head_(0),
      count_(0),
      seq_(0),
      printSeq_(0),
      rxFrames_(0),
      badFrames_(0),
      txFrames_(0),
      loopSec_(0),
      heapFree_(0),
      heapMin_(0),
      heapAtMs_(0),
      resetReason_(0),
      trace_(false),
      wifiGet_(nullptr),
      wifiSet_(nullptr),
      radio_(nullptr) {
  memset(ring_, 0, sizeof(ring_));
  memset(loopBucketUs_, 0, sizeof(loopBucketUs_));
  memset(dev_, 0, sizeof(dev_));
  for (uint8_t d = 0; d < DBG_DEVICE_COUNT; d++) {
    for (uint8_t i = 0; i < DIAG_KEY_COUNT; i++) {
      dev_[d].diag[i].key = DIAG_KEY_LIST[i];
      dev_[d].diag[i].valid = false;
    }
    dev_[d].diagCount = DIAG_KEY_COUNT;
  }
}

void Debug::begin() {
  resetReason_ = (int)esp_reset_reason();
  heapFree_ = ESP.getFreeHeap();
  heapMin_ = ESP.getMinFreeHeap();
  heapAtMs_ = millis();
  printSeq_ = oldestSeq();
  addEvent(DBG_EVT_BOOT, nullptr, 0);
}

uint32_t Debug::uptimeMs() const { return millis(); }

uint32_t Debug::rxRingDrops() const { return radio_ != nullptr ? radio_->rxRingDrops() : 0; }

uint32_t Debug::txFail(uint8_t dest) const {
  return radio_ != nullptr ? radio_->txFail(dest) : 0;
}

uint32_t Debug::loopMaxUs() const {
  uint32_t max = 0;
  for (uint8_t i = 0; i < 10; i++) {
    if (loopBucketUs_[i] > max) max = loopBucketUs_[i];
  }
  return max;
}

const char* Debug::resetReasonText() const { return resetText(resetReason_); }

uint8_t Debug::deviceIndex(uint8_t id) {
  if (id >= ID_STICK1 && id <= ID_STICK4) return (uint8_t)(id - ID_STICK1);
  if (id == ID_DISPLAY) return RTD_MAX_PLAYERS;
  return DBG_DEVICE_NONE;
}

void Debug::addEvent(uint8_t type, const uint8_t* payload, uint32_t u32) {
  DebugEvent& ev = ring_[head_];
  ev.seq = ++seq_;
  ev.ms = millis();
  ev.u32 = u32;
  ev.type = type;
  memset(ev.payload, 0, sizeof(ev.payload));
  if (payload != nullptr) memcpy(ev.payload, payload, sizeof(ev.payload));
  head_ = (uint8_t)((head_ + 1) % DBG_EVENT_COUNT);
  if (count_ < DBG_EVENT_COUNT) count_++;
}

bool Debug::nextEventAfter(uint32_t seq, DebugEvent& out) const {
  if (count_ == 0) return false;
  uint32_t oldest = seq_ - count_ + 1;
  uint32_t offset = (seq < oldest) ? 0 : seq - oldest + 1;
  if (offset >= count_) return false;
  uint8_t oldestIdx = (uint8_t)((head_ + DBG_EVENT_COUNT - count_) % DBG_EVENT_COUNT);
  out = ring_[(uint8_t)((oldestIdx + offset) % DBG_EVENT_COUNT)];
  return true;
}

void Debug::noteLoop(uint32_t us) {
  uint16_t sec = (uint16_t)(millis() / 1000);
  if (sec != loopSec_) {
    uint16_t gap = (uint16_t)(sec - loopSec_);
    if (gap >= 10) {
      memset(loopBucketUs_, 0, sizeof(loopBucketUs_));
    } else {
      for (uint16_t i = 1; i <= gap; i++) loopBucketUs_[(loopSec_ + i) % 10] = 0;
    }
    loopSec_ = sec;
  }
  uint8_t idx = (uint8_t)(sec % 10);
  if (us > loopBucketUs_[idx]) loopBucketUs_[idx] = us;
}

void Debug::diagStore(DebugDevice& dev, uint8_t key, uint8_t value) {
  for (uint8_t i = 0; i < dev.diagCount; i++) {
    if (dev.diag[i].key == key) {
      dev.diag[i].value = value;
      dev.diag[i].ms = millis();
      dev.diag[i].valid = true;
      return;
    }
  }
  if (dev.diagCount < DBG_DIAG_SLOTS) {
    DebugDiagValue& v = dev.diag[dev.diagCount++];
    v.key = key;
    v.value = value;
    v.ms = millis();
    v.valid = true;
  }
}

void Debug::noteRx(const uint8_t* data, uint8_t len, uint32_t us) {
  rxFrames_++;
  if (!validatePacket(data, (int)len)) {
    badFrames_++;
    return;
  }

  uint8_t src = data[2];
  uint8_t dest = data[1];
  uint8_t cmd = data[3];
  uint8_t hi = data[4];
  uint8_t lo = data[5];

  uint8_t idx = deviceIndex(src);
  if (idx != DBG_DEVICE_NONE) {
    DebugDevice& dev = dev_[idx];
    dev.seen = true;
    dev.lastMs = millis();
    dev.frames++;
    dev.lastCmd = cmd;
    if (cmd == CMD_DIAG) diagStore(dev, hi, lo);
  }

  if (!trace_ && quietCmd(cmd)) return;
  uint8_t payload[6] = {src, dest, cmd, hi, lo, 0};
  addEvent(DBG_EVT_RX, payload, us);
}

void Debug::noteTx(uint8_t destId, const uint8_t* data, uint8_t len) {
  txFrames_++;
  if (data == nullptr || len == 0) return;

  bool snapshot = (len == STATE_SIZE && data[0] == STATE_START);
  bool service = (len == SVC_SIZE && data[0] == SVC_START);
  uint8_t cmd = 0;
  uint8_t hi = 0;
  uint8_t lo = 0;
  if (service) {
    /* The service report is not a GamePacket; only the trace mode logs it. */
    cmd = CMD_SVC_REQ;
    hi = data[2];  /* seq */
    if (!trace_) return;
  } else if (!snapshot) {
    if (len != PACKET_SIZE || data[0] != PACKET_START) return;
    cmd = data[3];
    hi = data[4];
    lo = data[5];
    if (!trace_ && quietCmd(cmd)) return;
  } else if (!trace_) {
    return;
  }

  uint8_t kind = service ? 2 : (snapshot ? 1 : 0);
  uint8_t payload[6] = {destId, cmd, hi, lo, DBG_TX_OK_UNKNOWN, kind};
  addEvent(DBG_EVT_TX, payload, 0);
}

void Debug::notePhase(uint8_t oldPhase, uint8_t newPhase) {
  uint8_t payload[6] = {oldPhase, newPhase, 0, 0, 0, 0};
  addEvent(DBG_EVT_PHASE, payload, 0);
}

void Debug::noteJoined(uint8_t oldMask, uint8_t newMask) {
  uint8_t payload[6] = {oldMask, newMask, 0, 0, 0, 0};
  addEvent(DBG_EVT_JOINED, payload, 0);
}

/* -------------------------------------------------------- service report */

/* One DIAG_* value of a device; 0 and valid = false when it never arrived. */
static uint8_t diagValue(const DebugDevice& dev, uint8_t key, bool& valid) {
  for (uint8_t i = 0; i < dev.diagCount; i++) {
    if (dev.diag[i].key != key) continue;
    valid = dev.diag[i].valid;
    return valid ? dev.diag[i].value : 0;
  }
  valid = false;
  return 0;
}

void Debug::fillService(ServicePacket& sp, uint32_t nowMs) const {
  memset(&sp, 0, sizeof(sp));

  /* Host counters (DESIGN.md 12). */
  sp.uptime_min = rtdSvcClamp16(uptimeMs() / 60000u);
  sp.rx_frames = rtdSvcClamp16(rxFrames_);
  sp.rx_drop = rtdSvcClamp8(rxRingDrops());
  sp.bad_frame = rtdSvcClamp8(badFrames_);
  sp.tx_fail_bcast = rtdSvcClamp8(txFail(DBG_TX_DESTS - 1));
  sp.heap_kb = rtdSvcClamp8(heapFree_ / 1024u);
  sp.heap_min_kb = rtdSvcClamp8(heapMin_ / 1024u);
  sp.loop_max_ms = rtdSvcClamp8(loopMaxUs() / 1000u);
  sp.reset_reason = rtdSvcClamp8((uint32_t)resetReason_);
  if (trace_) sp.host_flags |= SVH_TRACE;

  /* One row per device: index 0..3 = stick ID 1..4, index 4 = display. */
  for (uint8_t i = 0; i < SVC_DEVICES; i++) {
    const DebugDevice& dev = dev_[i];
    ServiceDevice& out = sp.dev[i];
    bool isDisplay = (i >= RTD_MAX_PLAYERS);

    if (dev.seen) {
      out.flags |= SVD_SEEN;
      out.age_ds = rtdSvcAge((uint32_t)(nowMs - dev.lastMs));
    } else {
      out.age_ds = SVC_AGE_NEVER;
    }
    out.frames = rtdSvcClamp16(dev.frames);
    out.host_tx_fail = rtdSvcClamp8(txFail(i));

    /* The last value of each DIAG_* key, 0 when the device never sent it. */
    bool any = false;
    bool valid = false;
    out.fw = diagValue(dev, DIAG_FW_VERSION, valid);
    any = any || valid;
    out.reset_reason = diagValue(dev, DIAG_RESET_REASON, valid);
    any = any || valid;
    out.uptime_min = diagValue(dev, DIAG_UPTIME_MIN, valid);
    any = any || valid;
    out.heap_kb = diagValue(dev, DIAG_HEAP_KB, valid);
    any = any || valid;
    out.rx_drop = diagValue(dev, DIAG_RX_DROP, valid);
    any = any || valid;
    out.tx_fail = diagValue(dev, DIAG_TX_FAIL, valid);
    any = any || valid;
    out.bad_frame = diagValue(dev, DIAG_BAD_FRAME, valid);
    any = any || valid;
    out.loop_max_ms = diagValue(dev, DIAG_LOOP_MAX_MS, valid);
    any = any || valid;
    out.link_gap_ds = diagValue(dev, DIAG_LINK_GAP_DS, valid);
    any = any || valid;
    out.extra = diagValue(dev, isDisplay ? DIAG_SNAP_PER_10S : DIAG_STICK_STATE, valid);
    any = any || valid;

    uint8_t sensor = diagValue(dev, isDisplay ? DIAG_TOUCH_OK : DIAG_MPU_OK, valid);
    any = any || valid;
    if (valid && sensor == 1) out.flags |= SVD_SENSOR_OK;

    if (any) out.flags |= SVD_DIAG;
  }
}

/* ------------------------------------------------------------ formatting */

const char* Debug::cmdName(uint8_t cmd) {
  switch (cmd) {
    case CMD_OK:             return "OK";
    case CMD_REQ_ID:         return "REQ_ID";
    case CMD_ACK:            return "ACK";
    case CMD_GAME_START:     return "GAME_START";
    case CMD_GO:             return "GO";
    case CMD_VIBRATE:        return "VIBRATE";
    case CMD_IDLE:           return "IDLE";
    case CMD_COUNTDOWN:      return "COUNTDOWN";
    case CMD_REACTION_DONE:  return "REACTION_DONE";
    case CMD_SHAKE_DONE:     return "SHAKE_DONE";
    case CMD_SHAKE_PROGRESS: return "SHAKE_PROGRESS";
    case CMD_BEACON:         return "BEACON";
    case CMD_HEARTBEAT:      return "HEARTBEAT";
    case CMD_CFG_SET:        return "CFG_SET";
    case CMD_ACTION:         return "ACTION";
    case CMD_DISPLAY_HELLO:  return "DISPLAY_HELLO";
    case CMD_OTA_BEGIN:      return "OTA_BEGIN";
    case CMD_OTA_STATUS:     return "OTA_STATUS";
    case CMD_DISPLAY_INFO:   return "DISPLAY_INFO";
    case CMD_DIAG:           return "DIAG";
    case CMD_SVC_REQ:        return "SVC_REQ";
    default:                 return nullptr;
  }
}

const char* Debug::phaseName(uint8_t phase) {
  switch (phase) {
    case PH_IDLE:       return "IDLE";
    case PH_JOIN:       return "JOIN";
    case PH_ANNOUNCE:   return "ANNOUNCE";
    case PH_COUNTDOWN:  return "COUNTDOWN";
    case PH_REACT_WAIT: return "REACT_WAIT";
    case PH_GO:         return "GO";
    case PH_SHAKE:      return "SHAKE";
    case PH_RESULTS:    return "RESULTS";
    case PH_FINAL:      return "FINAL";
    case PH_MAINT:      return "MAINT";
    default:            return "?";
  }
}

const char* Debug::deviceName(uint8_t id) {
  switch (id) {
    case ID_HOST:      return "HOST";
    case ID_STICK1:    return "S1";
    case ID_STICK2:    return "S2";
    case ID_STICK3:    return "S3";
    case ID_STICK4:    return "S4";
    case ID_DISPLAY:   return "DISP";
    case ID_BROADCAST: return "ALL";
    default:           return "?";
  }
}

void Debug::formatEvent(const DebugEvent& ev, char* buf, size_t len) const {
  if (buf == nullptr || len == 0) return;
  int n = snprintf(buf, len, "#%lu %lu ", (unsigned long)ev.seq, (unsigned long)ev.ms);
  if (n < 0) return;
  size_t used = (size_t)n < len ? (size_t)n : len - 1;
  const uint8_t* p = ev.payload;

  switch (ev.type) {
    case DBG_EVT_BOOT:
      snprintf(buf + used, len - used, "BOOT");
      break;
    case DBG_EVT_PHASE:
      snprintf(buf + used, len - used, "PHASE %s>%s", phaseName(p[0]), phaseName(p[1]));
      break;
    case DBG_EVT_JOINED:
      snprintf(buf + used, len - used, "JOINED 0x%02X>0x%02X", p[0], p[1]);
      break;
    case DBG_EVT_RX: {
      char cmd[20];
      cmdText(cmd, sizeof(cmd), p[2]);
      snprintf(buf + used, len - used, "RX %s>%s %s hi=%u lo=%u us=%lu",
               deviceName(p[0]), deviceName(p[1]), cmd, (unsigned)p[3], (unsigned)p[4],
               (unsigned long)ev.u32);
      break;
    }
    case DBG_EVT_TX: {
      char cmd[20];
      if (p[5] == 1) {
        snprintf(cmd, sizeof(cmd), "SNAPSHOT");
      } else if (p[5] == 2) {
        snprintf(cmd, sizeof(cmd), "SERVICE");
      } else {
        cmdText(cmd, sizeof(cmd), p[1]);
      }
      const char* ok = (p[4] == 0) ? "fail" : (p[4] == 1 ? "ok" : "?");
      snprintf(buf + used, len - used, "TX %s %s hi=%u lo=%u ok=%s", deviceName(p[0]), cmd,
               (unsigned)p[2], (unsigned)p[3], ok);
      break;
    }
    default:
      snprintf(buf + used, len - used, "TYPE %u", (unsigned)ev.type);
      break;
  }
}

/* ---------------------------------------------------------------- serial */

bool Debug::wifiOn() { return wifiGet_ != nullptr ? wifiGet_() : false; }

void Debug::toggleWifi() {
  if (wifiSet_ == nullptr) return;
  bool on = wifiOn();
  wifiSet_(!on);
  if (on) {
    Serial.println("[DBG] debug_wifi off (the access point stays on until the next boot)");
  } else {
    Serial.println("[DBG] debug_wifi on (the access point starts at the next boot)");
  }
}

void Debug::run() {
  uint32_t now = millis();
  if ((uint32_t)(now - heapAtMs_) >= 1000) {
    heapAtMs_ = now;
    heapFree_ = ESP.getFreeHeap();
    uint32_t minHeap = ESP.getMinFreeHeap();
    if (minHeap < heapMin_) heapMin_ = minHeap;
  }
  handleSerial();
  flushSerial();
}

void Debug::handleSerial() {
  /* At most 8 bytes per pass, so a dump never stalls the loop. */
  uint8_t read = 0;
  while (read < 8 && Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    read++;
    switch (c) {
      case '?':
        printHelp();
        break;
      case 's':
        printStatus();
        break;
      case 'l':
        printSeq_ = oldestSeq();
        if (count_ == 0) Serial.println("[DBG] log empty");
        break;
      case 't':
        trace_ = !trace_;
        Serial.printf("[DBG] trace %s\n", trace_ ? "on" : "off");
        break;
      case 'w':
        toggleWifi();
        break;
      default:
        break;  /* CR, LF and unknown bytes are ignored */
    }
  }
}

void Debug::flushSerial() {
  /* At most 4 event lines per pass; the ring keeps the rest. */
  uint8_t printed = 0;
  while (printed < 4 && count_ > 0) {
    uint32_t oldest = seq_ - count_ + 1;
    if (printSeq_ < oldest) printSeq_ = oldest;
    if (printSeq_ > seq_) return;
    uint8_t oldestIdx = (uint8_t)((head_ + DBG_EVENT_COUNT - count_) % DBG_EVENT_COUNT);
    uint8_t idx = (uint8_t)((oldestIdx + (printSeq_ - oldest)) % DBG_EVENT_COUNT);
    char line[DBG_EVENT_TEXT_MAX];
    formatEvent(ring_[idx], line, sizeof(line));
    /* Never wait for the UART: a burst of events must not delay the game
     * loop. The line stays in the ring until the transmit buffer has room. */
    if ((size_t)Serial.availableForWrite() < strlen(line) + 8) return;
    Serial.printf("[EVT] %s\n", line);
    printSeq_++;
    printed++;
  }
}

void Debug::printHelp() {
  Serial.println("[DBG] ?  this help");
  Serial.println("[DBG] s  status (counters and devices)");
  Serial.println("[DBG] l  print the whole event log");
  Serial.println("[DBG] t  trace on/off (beacon, heartbeat, diag, shake progress, snapshot)");
  Serial.println("[DBG] w  debug Wi-Fi on/off");
}

void Debug::printStatus() {
  uint32_t now = millis();
  Serial.printf("[DBG] uptime %lu s  heap %lu/%lu  reset %d (%s)  trace %s  debug_wifi %s\n",
                (unsigned long)(now / 1000), (unsigned long)heapFree_,
                (unsigned long)heapMin_, resetReason_, resetReasonText(),
                trace_ ? "on" : "off", wifiOn() ? "on" : "off");
  Serial.printf("[DBG] rx %lu  ring drops %lu  bad %lu  tx %lu  tx fail S1..S4 %lu/%lu/%lu/%lu "
                "disp %lu bcast %lu  loop max %lu us\n",
                (unsigned long)rxFrames_, (unsigned long)rxRingDrops(),
                (unsigned long)badFrames_, (unsigned long)txFrames_,
                (unsigned long)txFail(0), (unsigned long)txFail(1),
                (unsigned long)txFail(2), (unsigned long)txFail(3),
                (unsigned long)txFail(RTD_MAX_PLAYERS),
                (unsigned long)txFail(DBG_TX_DESTS - 1), (unsigned long)loopMaxUs());
  for (uint8_t i = 0; i < DBG_DEVICE_COUNT; i++) printDevice(i);
}

void Debug::printDevice(uint8_t idx) {
  const DebugDevice& dev = dev_[idx];
  const char* label = DEVICE_LABELS[idx];

  if (!dev.seen) {
    Serial.printf("[DBG] %s no frames\n", label);
    return;
  }

  const char* cmd = cmdName(dev.lastCmd);
  if (cmd != nullptr) {
    Serial.printf("[DBG] %s last %lu ms ago, %lu frames, last cmd %s\n", label,
                  (unsigned long)(millis() - dev.lastMs), (unsigned long)dev.frames, cmd);
  } else {
    Serial.printf("[DBG] %s last %lu ms ago, %lu frames, last cmd 0x%02X\n", label,
                  (unsigned long)(millis() - dev.lastMs), (unsigned long)dev.frames,
                  dev.lastCmd);
  }

  char line[DBG_EVENT_TEXT_MAX];
  size_t used = 0;
  uint32_t now = millis();
  for (uint8_t i = 0; i < dev.diagCount; i++) {
    if (!dev.diag[i].valid) continue;
    int n = snprintf(line + used, sizeof(line) - used, "%s%s=%u(%lums)",
                     used == 0 ? "" : " ", rtdDiagKeyName(dev.diag[i].key),
                     (unsigned)dev.diag[i].value,
                     (unsigned long)(now - dev.diag[i].ms));
    if (n < 0) break;
    if ((size_t)n >= sizeof(line) - used) {
      /* Line full: print the complete values and start again with this one. */
      line[used] = '\0';
      Serial.printf("[DBG]   %s\n", line);
      used = 0;
      n = snprintf(line, sizeof(line), "%s=%u(%lums)", rtdDiagKeyName(dev.diag[i].key),
                   (unsigned)dev.diag[i].value, (unsigned long)(now - dev.diag[i].ms));
      if (n < 0) break;
    }
    used += (size_t)n;
  }
  if (used > 0) Serial.printf("[DBG]   %s\n", line);
}
