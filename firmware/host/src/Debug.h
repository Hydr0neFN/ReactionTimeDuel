#ifndef RTD_DEBUG_H
#define RTD_DEBUG_H

/*
 * Host debug hub (DESIGN.md 11.2): counters, a per-device table for the
 * CMD_DIAG reports and a fixed-size event ring. The serial console and the
 * web page read the same data. Everything here runs on the loop task; the
 * ESP-NOW callbacks only bump their own volatile counters (Radio.h).
 *
 * No heap, no String per event: the ring has fixed-size entries and one
 * formatter writes an event as one text line into a caller buffer.
 */

#include <stdint.h>
#include <stddef.h>
#include "RtdProtocol.h"
#include "RtdDiag.h"
#include "RtdService.h"

#define DBG_EVENT_COUNT    200
#define DBG_EVENT_TEXT_MAX 128
#define DBG_DEVICE_COUNT   5   /* sticks 1..4, then the display */
#define DBG_DEVICE_NONE    0xFF
#define DBG_DIAG_SLOTS     24  /* the 22 known keys plus room for new ones */
#define DBG_TX_DESTS       6   /* sticks 1..4, display, broadcast */

/* Event types. */
#define DBG_EVT_BOOT   0
#define DBG_EVT_PHASE  1
#define DBG_EVT_JOINED 2
#define DBG_EVT_RX     3
#define DBG_EVT_TX     4

/* TX result flag in the payload; the ESP-NOW send callback is asynchronous. */
#define DBG_TX_OK_UNKNOWN 0xFF

struct DebugEvent {
  uint32_t seq;
  uint32_t ms;
  uint32_t u32;        /* RX: the micros() stamp */
  uint8_t  type;
  uint8_t  payload[6]; /* PHASE old/new, JOINED old/new, RX src/dest/cmd/hi/lo,
                          TX dest/cmd/hi/lo/ok/kind (0 packet, 1 snapshot, 2 service) */
};

struct DebugDiagValue {
  uint8_t  key;
  uint8_t  value;
  uint32_t ms;
  bool     valid;
};

struct DebugDevice {
  uint32_t lastMs;
  uint32_t frames;
  uint8_t  lastCmd;
  bool     seen;
  uint8_t  diagCount;
  DebugDiagValue diag[DBG_DIAG_SLOTS];
};

class Radio;

class Debug {
public:
  Debug();

  /* Capture the reset reason and add the BOOT event. */
  void begin();

  /* Counters of the radio callbacks come from here. */
  void setRadio(Radio* radio) { radio_ = radio; }

  /* One loop pass: 1 Hz heap sample, serial console, up to 4 [EVT] lines. */
  void run();

  /* Duration of one loop() pass; the max over the last 10 s is kept. */
  void noteLoop(uint32_t us);

  /* A frame popped from the radio ring: counters, device table, event. */
  void noteRx(const uint8_t* data, uint8_t len, uint32_t us);

  /* A frame handed to the radio: counters and (unless quiet) the event. */
  void noteTx(uint8_t destId, const uint8_t* data, uint8_t len);

  void notePhase(uint8_t oldPhase, uint8_t newPhase);
  void noteJoined(uint8_t oldMask, uint8_t newMask);

  void setTrace(bool on) { trace_ = on; }
  bool trace() const { return trace_; }

  /* The serial 'w' command and the web page share the stored setting. */
  typedef bool (*BoolGetter)();
  typedef void (*BoolSetter)(bool);
  void setWifiHooks(BoolGetter get, BoolSetter set) {
    wifiGet_ = get;
    wifiSet_ = set;
  }

  /* ---- counters ---- */
  uint32_t uptimeMs() const;
  uint32_t rxFrames() const { return rxFrames_; }
  uint32_t rxRingDrops() const;
  uint32_t badFrames() const { return badFrames_; }
  uint32_t txFrames() const { return txFrames_; }
  uint32_t txFail(uint8_t dest) const;
  uint32_t loopMaxUs() const;
  uint32_t heapFree() const { return heapFree_; }
  uint32_t heapMin() const { return heapMin_; }
  int      resetReason() const { return resetReason_; }
  const char* resetReasonText() const;

  /* ---- per-device table (0..3 = stick ID 1..4, 4 = display) ---- */
  const DebugDevice& device(uint8_t idx) const {
    return dev_[idx < DBG_DEVICE_COUNT ? idx : 0];
  }
  static uint8_t deviceIndex(uint8_t id);

  /* ---- service report (DESIGN.md 12) ---- */
  /* Zero the frame and fill the host counters and the device table. The
   * caller adds seq, the flags that need storage / Wi-Fi and ap_clients,
   * then seals the frame with sealService(). */
  void fillService(ServicePacket& sp, uint32_t nowMs) const;

  /* ---- event ring ---- */
  uint32_t oldestSeq() const { return count_ == 0 ? 0 : seq_ - count_ + 1; }
  uint32_t newestSeq() const { return seq_; }

  /* Next event with a sequence number greater than seq; false when done. */
  bool nextEventAfter(uint32_t seq, DebugEvent& out) const;

  /* One event as one text line (no newline); serial and /api/log share it. */
  void formatEvent(const DebugEvent& ev, char* buf, size_t len) const;

  static const char* cmdName(uint8_t cmd);
  static const char* phaseName(uint8_t phase);
  static const char* deviceName(uint8_t id);

private:
  void addEvent(uint8_t type, const uint8_t* payload, uint32_t u32);
  void diagStore(DebugDevice& dev, uint8_t key, uint8_t value);
  void handleSerial();
  void flushSerial();
  void printHelp();
  void printStatus();
  void printDevice(uint8_t idx);
  void toggleWifi();
  bool wifiOn();

  DebugEvent ring_[DBG_EVENT_COUNT];
  uint8_t  head_;      /* next write index */
  uint8_t  count_;
  uint32_t seq_;
  uint32_t printSeq_;  /* next event the serial port prints */

  DebugDevice dev_[DBG_DEVICE_COUNT];

  uint32_t rxFrames_;
  uint32_t badFrames_;
  uint32_t txFrames_;

  uint32_t loopBucketUs_[10];
  uint16_t loopSec_;
  uint32_t heapFree_;
  uint32_t heapMin_;
  uint32_t heapAtMs_;

  int      resetReason_;
  bool     trace_;
  BoolGetter wifiGet_;
  BoolSetter wifiSet_;
  Radio*   radio_;
};

extern Debug debug;

#endif /* RTD_DEBUG_H */
