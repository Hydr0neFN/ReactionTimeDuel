#include "DisplayLink.h"

#include <string.h>

DisplayLink::DisplayLink(DisplayPorts& ports)
    : ports_(ports),
      have_(false),
      lastRxMs_(0),
      helloMs_(0),
      helloSent_(false) {
  memset(&last_, 0, sizeof(last_));
  for (uint8_t k = 0; k < CFG_SLOTS; k++) {
    pending_[k] = 0;
    pendingActive_[k] = false;
    sendCount_[k] = 0;
    nextSendMs_[k] = 0;
  }
}

void DisplayLink::sendPacket(uint8_t cmd, uint16_t data) {
  GamePacket pkt;
  buildPacket(&pkt, ID_HOST, ID_DISPLAY, cmd, data);
  ports_.send((const uint8_t*)&pkt, PACKET_SIZE);
}

bool DisplayLink::onPacket(const uint8_t* data, int len, uint32_t rxMs) {
  if (!validateState(data, len)) return false;
  memcpy(&last_, data, STATE_SIZE);
  have_ = true;
  lastRxMs_ = rxMs;
  return true;
}

bool DisplayLink::linkUp() {
  if (!have_) return false;
  return (uint32_t)(ports_.millis() - lastRxMs_) <= LINK_TIMEOUT_MS;
}

void DisplayLink::sendHello() {
  sendPacket(CMD_DISPLAY_HELLO, ((uint16_t)FW_VERSION_BYTE << 8) | FW_VERSION_PATCH);
}

void DisplayLink::sendInfo(uint8_t lastIpOctet, uint8_t otaState) {
  sendPacket(CMD_DISPLAY_INFO, ((uint16_t)lastIpOctet << 8) | otaState);
}

void DisplayLink::sendAction(uint8_t action, uint8_t arg) {
  sendPacket(CMD_ACTION, ((uint16_t)action << 8) | arg);
}

void DisplayLink::setCfg(uint8_t key, uint8_t value) {
  if (key < 1 || key > CFG_COUNT) return;
  pending_[key] = value;
  pendingActive_[key] = true;
  sendCount_[key] = 1;
  uint32_t now = ports_.millis();
  nextSendMs_[key] = now;
  sendPacket(CMD_CFG_SET, ((uint16_t)key << 8) | value);
}

uint8_t DisplayLink::cfg(uint8_t key) const {
  if (key < 1 || key > CFG_COUNT) return 0;
  if (pendingActive_[key]) return pending_[key];
  if (!have_) return 0;
  return last_.cfg[key];
}

bool DisplayLink::cfgPending(uint8_t key) const {
  if (key < 1 || key > CFG_COUNT) return false;
  return pendingActive_[key];
}

void DisplayLink::tick() {
  uint32_t now = ports_.millis();

  if (!helloSent_) {
    helloSent_ = true;
    helloMs_ = now;
    sendHello();
  } else if ((uint32_t)(now - helloMs_) >= HELLO_INTERVAL_MS) {
    helloMs_ = now;
    sendHello();
  }

  for (uint8_t key = 1; key <= CFG_COUNT; key++) {
    if (!pendingActive_[key]) continue;
    if (have_ && last_.cfg[key] == pending_[key]) {
      pendingActive_[key] = false;
      continue;
    }
    if ((uint32_t)(now - nextSendMs_[key]) < CFG_RESEND_MS) continue;
    if (sendCount_[key] >= CFG_MAX_SENDS) {
      pendingActive_[key] = false;
      continue;
    }
    sendCount_[key]++;
    nextSendMs_[key] = now;
    sendPacket(CMD_CFG_SET, ((uint16_t)key << 8) | pending_[key]);
  }
}
