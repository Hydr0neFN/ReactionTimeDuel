#ifndef RTD_RADIO_H
#define RTD_RADIO_H

/*
 * ESP-NOW transport (DESIGN.md 3): channel 6, power save off, address learning
 * for sticks and the display. The receive callback only copies the frame, the
 * sender MAC and the micros() timestamp into a fixed-size ring; all other work
 * runs in the main loop.
 */

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include "RtdProtocol.h"

#define RADIO_RING_SIZE 32

/* Diagnostic classes of the send callback: sticks 1..4, display, broadcast. */
#define RADIO_TX_CLASSES 6

struct RxFrame {
  uint32_t us;                 /* micros() at reception */
  uint8_t  len;
  uint8_t  mac[6];
  uint8_t  data[PACKET_SIZE];
};

class Radio {
public:
  Radio();

  /* STA mode, channel 6, no power save, maximum TX power, ESP-NOW, default peers. */
  void begin();

  /* Send to a device ID (sticks, display) or ID_BROADCAST. */
  void send(uint8_t destId, const uint8_t* data, uint8_t len);

  /* Pop one received frame (main loop). Address learning happens here. */
  bool pop(RxFrame& out);

  /* Current peer MAC for a device ID (nullptr for ID_BROADCAST / unknown). */
  const uint8_t* macFor(uint8_t devId) const;

  /* Called from the ESP-NOW receive callback. */
  void onRecv(const uint8_t* mac, const uint8_t* data, int len);

  /* Called from the ESP-NOW send callback (diagnostics only). */
  void onSendResult(const uint8_t* mac, bool ok);

  /* Diagnostic counters, written by the callbacks only (DESIGN.md 11.2). */
  uint32_t rxRingDrops() const { return rxDrops_; }
  uint32_t txFail(uint8_t destClass) const;
  uint32_t txDone(uint8_t destClass) const;

private:
  void addPeer(const uint8_t* mac);
  void learn(const RxFrame& frame);
  void replacePeer(uint8_t* stored, const uint8_t* mac);
  bool macInUse(const uint8_t* mac, const uint8_t* except) const;

  uint8_t  stickMac_[RTD_MAX_PLAYERS][6];
  uint8_t  displayMac_[6];
  uint8_t  broadcastMac_[6];

  RxFrame  ring_[RADIO_RING_SIZE];
  volatile uint8_t head_;
  volatile uint8_t tail_;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;

  volatile uint32_t rxDrops_;
  volatile uint32_t txDone_[RADIO_TX_CLASSES];
  volatile uint32_t txFail_[RADIO_TX_CLASSES];
};

#endif /* RTD_RADIO_H */
