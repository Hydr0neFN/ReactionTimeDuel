#include "Radio.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

#include "RtdNetConfig.h"

static const uint8_t DEFAULT_STICK_MAC[RTD_MAX_PLAYERS][6] = {
    RTD_MAC_STICK1, RTD_MAC_STICK2, RTD_MAC_STICK3, RTD_MAC_STICK4};
static const uint8_t DEFAULT_DISPLAY_MAC[6] = RTD_MAC_DISPLAY;
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static Radio* s_radio = nullptr;

/* ESP-NOW entry point: kept in IRAM, no Serial, no blocking, no allocation. */
static void IRAM_ATTR recvCb(const uint8_t* mac, const uint8_t* data, int len) {
  if (s_radio) s_radio->onRecv(mac, data, len);
}

/* Send result for the diagnostics only: a volatile counter bump per class. */
static void IRAM_ATTR sendCb(const uint8_t* mac, esp_now_send_status_t status) {
  if (s_radio) s_radio->onSendResult(mac, status == ESP_NOW_SEND_SUCCESS);
}

Radio::Radio() : head_(0), tail_(0), rxDrops_(0) {
  memcpy(stickMac_, DEFAULT_STICK_MAC, sizeof(stickMac_));
  memcpy(displayMac_, DEFAULT_DISPLAY_MAC, sizeof(displayMac_));
  memcpy(broadcastMac_, BROADCAST_MAC, sizeof(broadcastMac_));
  for (uint8_t i = 0; i < RADIO_TX_CLASSES; i++) {
    txDone_[i] = 0;
    txFail_[i] = 0;
  }
}

void Radio::begin() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_max_tx_power(84);  /* 21 dBm */

  if (esp_now_init() != ESP_OK) {
    Serial.println("[RADIO] ESP-NOW init failed");
    return;
  }
  s_radio = this;
  esp_now_register_recv_cb(recvCb);
  esp_now_register_send_cb(sendCb);

  addPeer(broadcastMac_);
  addPeer(displayMac_);
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) addPeer(stickMac_[i]);

  Serial.printf("[RADIO] ESP-NOW ready, channel %d, host MAC %s\n", ESPNOW_CHANNEL,
                WiFi.macAddress().c_str());
}

void Radio::addPeer(const uint8_t* mac) {
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;
  /* A learned address can already be registered by another slot. */
  esp_err_t err = esp_now_add_peer(&peer);
  if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf("[RADIO] add peer failed: %d\n", (int)err);
  }
}

const uint8_t* Radio::macFor(uint8_t devId) const {
  if (devId >= ID_STICK1 && devId <= ID_STICK4) return stickMac_[devId - ID_STICK1];
  if (devId == ID_DISPLAY) return displayMac_;
  return nullptr;
}

void Radio::send(uint8_t destId, const uint8_t* data, uint8_t len) {
  const uint8_t* mac = macFor(destId);
  if (mac == nullptr) mac = broadcastMac_;
  esp_now_send(mac, data, len);
}

/* True when a slot other than "except" still holds this MAC. The broadcast
 * peer has no slot and is never deleted. */
bool Radio::macInUse(const uint8_t* mac, const uint8_t* except) const {
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (stickMac_[i] != except && memcmp(stickMac_[i], mac, 6) == 0) return true;
  }
  if (displayMac_ != except && memcmp(displayMac_, mac, 6) == 0) return true;
  return false;
}

void Radio::replacePeer(uint8_t* stored, const uint8_t* mac) {
  if (memcmp(stored, mac, 6) == 0) return;

  /* A stick flashed with another ID makes two slots share one MAC; keep the
   * peer registered as long as any slot still needs it. */
  if (!macInUse(stored, stored)) esp_now_del_peer(stored);
  memcpy(stored, mac, 6);

  /* The other slot(s) that held the new MAC go back to their default MAC. */
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (stickMac_[i] != stored && memcmp(stickMac_[i], mac, 6) == 0) {
      memcpy(stickMac_[i], DEFAULT_STICK_MAC[i], 6);
      addPeer(stickMac_[i]);
    }
  }

  addPeer(stored);
  Serial.printf("[RADIO] Learned %02X:%02X:%02X:%02X:%02X:%02X\n", stored[0], stored[1],
                stored[2], stored[3], stored[4], stored[5]);
}

void Radio::learn(const RxFrame& frame) {
  if (!validatePacket(frame.data, frame.len)) return;
  uint8_t src = frame.data[2];  /* src_id */
  if (src >= ID_STICK1 && src <= ID_STICK4) {
    replacePeer(stickMac_[src - ID_STICK1], frame.mac);
  } else if (src == ID_DISPLAY) {
    replacePeer(displayMac_, frame.mac);
  }
}

bool Radio::pop(RxFrame& out) {
  bool have = false;
  portENTER_CRITICAL(&mux_);
  if (tail_ != head_) {
    out = ring_[tail_];
    tail_ = (uint8_t)((tail_ + 1) % RADIO_RING_SIZE);
    have = true;
  }
  portEXIT_CRITICAL(&mux_);
  if (have) learn(out);
  return have;
}

void Radio::onRecv(const uint8_t* mac, const uint8_t* data, int len) {
  if (len <= 0 || len > PACKET_SIZE) return;
  uint32_t us = micros();

  portENTER_CRITICAL(&mux_);
  uint8_t next = (uint8_t)((head_ + 1) % RADIO_RING_SIZE);
  if (next != tail_) {
    RxFrame& frame = ring_[head_];
    frame.us = us;
    frame.len = (uint8_t)len;
    memcpy(frame.mac, mac, 6);
    memcpy(frame.data, data, (size_t)len);
    head_ = next;
  } else {
    rxDrops_++;
  }
  portEXIT_CRITICAL(&mux_);
}

void Radio::onSendResult(const uint8_t* mac, bool ok) {
  uint8_t cls = RADIO_TX_CLASSES - 1;  /* broadcast, also the unknown address */
  if (mac != nullptr) {
    for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
      if (memcmp(stickMac_[i], mac, 6) == 0) {
        cls = i;
        break;
      }
    }
    if (cls == RADIO_TX_CLASSES - 1 && memcmp(displayMac_, mac, 6) == 0) {
      cls = RTD_MAX_PLAYERS;
    }
  }
  if (ok) {
    txDone_[cls]++;
  } else {
    txFail_[cls]++;
  }
}

uint32_t Radio::txFail(uint8_t destClass) const {
  return destClass < RADIO_TX_CLASSES ? txFail_[destClass] : 0;
}

uint32_t Radio::txDone(uint8_t destClass) const {
  return destClass < RADIO_TX_CLASSES ? txDone_[destClass] : 0;
}
