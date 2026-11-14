#ifndef RTD_DISPLAY_LINK_H
#define RTD_DISPLAY_LINK_H

/*
 * DisplayLink - display side of the host link (DESIGN.md 7.1).
 *
 * Validates the StatePacket snapshots of the host, keeps the last good one and
 * carries the setting sync towards the host. Plain C++11, no heap.
 */

#include <stdint.h>
#include "RtdProtocol.h"

#define LINK_TIMEOUT_MS     2000   /* a good snapshot this recent means the link is up */
#define HELLO_INTERVAL_MS   2000   /* CMD_DISPLAY_HELLO period */
#define CFG_RESEND_MS       400    /* setting resend interval */
#define CFG_MAX_SENDS       5      /* total transmissions per setting change */

/*
 * DisplayPorts - everything DisplayLink needs from the device.
 */
class DisplayPorts {
public:
  virtual ~DisplayPorts() {}

  /* Monotonic milliseconds since boot, may wrap (compare with unsigned subtraction). */
  virtual uint32_t millis() = 0;

  /* Send one frame to the host. */
  virtual void send(const uint8_t* data, uint8_t len) = 0;
};

class DisplayLink {
public:
  /* Bind the link to its ports; the first hello goes out on the first tick(). */
  explicit DisplayLink(DisplayPorts& ports);

  /* Handle one received frame; true when it was a valid, newer snapshot. */
  bool onPacket(const uint8_t* data, int len, uint32_t rxMs);

  /* True when a good snapshot arrived within the last LINK_TIMEOUT_MS. */
  bool linkUp();

  /* True when at least one snapshot has been received. */
  bool hasState() const { return have_; }

  /* Last good snapshot (all zero before the first one arrives). */
  const StatePacket& state() const { return last_; }

  /* Run the setting resends and the hello timer; call from the UI timer. */
  void tick();

  /* Request a setting value; sends at once and resends until the host agrees. */
  void setCfg(uint8_t key, uint8_t value);

  /* Value shown on the screen: the pending value while a send is pending, else the snapshot value. */
  uint8_t cfg(uint8_t key) const;

  /* True while a setting change is still being sent to the host. */
  bool cfgPending(uint8_t key) const;

  /* Send CMD_ACTION with its argument. */
  void sendAction(uint8_t action, uint8_t arg);

  /* Send CMD_DISPLAY_HELLO at once. */
  void sendHello();

  /* Send CMD_DISPLAY_INFO (maintenance mode, every 2000 ms by the caller). */
  void sendInfo(uint8_t lastIpOctet, uint8_t otaState);

private:
  void sendPacket(uint8_t cmd, uint16_t data);

  DisplayPorts& ports_;
  StatePacket last_;
  bool have_;
  uint32_t lastRxMs_;
  uint32_t helloMs_;
  bool helloSent_;

  uint8_t pending_[CFG_SLOTS];
  bool pendingActive_[CFG_SLOTS];
  uint8_t sendCount_[CFG_SLOTS];
  uint32_t nextSendMs_[CFG_SLOTS];
};

#endif /* RTD_DISPLAY_LINK_H */
