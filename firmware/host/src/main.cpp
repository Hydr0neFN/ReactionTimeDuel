/*
 * Reaction Time Duel - host firmware V4.0.0 (ESP32 DevKit-C)
 *
 * All game logic lives in shared/rtd_core (HostGame); this project only
 * implements the hardware ports: ESP-NOW radio, NeoPixel rings, WS2812B
 * strip, MP3 audio, NVS settings and the OTA maintenance mode.
 *
 * Main loop (DESIGN.md 5.3, H3):
 *   drain the ESP-NOW receive ring -> Debug::noteRx() -> HostGame::onPacket()
 *   HostGame::tick()  (timers, retries, beacons, the timed snapshot send)
 *   LEDs, audio, settings debounce, web server
 *
 * Debug hub (DESIGN.md 11.2): every received and sent frame is counted and,
 * except for the quiet commands and the snapshot, logged as one event.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <string.h>

#include "RtdProtocol.h"
#include "RtdService.h"
#include "HostGame.h"
#include "Board.h"
#include "Radio.h"
#include "Leds.h"
#include "Audio.h"
#include "Storage.h"
#include "Maint.h"
#include "Debug.h"

/* Printed at boot so the linker keeps the marker in the image (DESIGN.md 9.2). */
static const char FW_MARKER[] = RTD_FW_MARKER(host);

static Radio radio;
static Leds leds;
static Audio audio;
static Storage storage;

class Ports : public HostPorts {
public:
  Maint* maint = nullptr;

  uint32_t millis() override { return ::millis(); }
  uint32_t micros() override { return ::micros(); }

  /* H12: hardware RNG, not an ADC pin. */
  uint32_t random(uint32_t n) override {
    return n == 0 ? 0 : (uint32_t)(esp_random() % n);
  }

  void send(uint8_t destId, const uint8_t* data, uint8_t len) override {
    radio.send(destId, data, len);
    debug.noteTx(destId, data, len);
  }

  void audioPlay(RtdSound s) override { audio.play(s); }
  void audioStop() override { audio.stop(); }
  bool audioIdle() override { return audio.idle(); }

  void applySettings(const uint8_t* cfg) override {
    leds.applySettings(cfg);
    audio.setVolume(cfg[CFG_VOLUME]);
  }

  void saveSettings(const uint8_t* cfg) override { storage.saveSettings(cfg); }
  void saveStats(uint16_t bestAlltimeMs, uint16_t gamesTotal) override {
    storage.saveStats(bestAlltimeMs, gamesTotal);
  }

  bool stickImage(uint8_t* ver, uint8_t* patch) override {
    return maint != nullptr && maint->stickImage(ver, patch);
  }

  void maintEnter() override {
    if (maint != nullptr) maint->enter();
  }

  void maintExit() override {
    if (maint != nullptr) maint->exit();
  }
};

static Ports ports;
static HostGame game(ports);
static Maint maint(game, storage, debug);

/* The debug Wi-Fi switch lives in NVS (DESIGN.md 11.3). */
static bool readDebugWifi() { return storage.debugWifi(); }
static void writeDebugWifi(bool on) { storage.setDebugWifi(on); }

/* Display version / address arrive as plain game packets; keep them for the page. */
static void sniffDisplay(const RxFrame& frame) {
  if (!validatePacket(frame.data, frame.len)) return;

  GamePacket pkt;
  memcpy(&pkt, frame.data, PACKET_SIZE);
  if (pkt.src_id != ID_DISPLAY) return;

  uint32_t now = millis();
  if (pkt.cmd == CMD_DISPLAY_HELLO) {
    maint.display.ver = pkt.data_high;
    maint.display.patch = pkt.data_low;
    maint.display.seen = true;
    maint.display.lastSeenMs = now;
  } else if (pkt.cmd == CMD_DISPLAY_INFO) {
    maint.display.ipOctet = pkt.data_high;
    maint.display.otaState = pkt.data_low;
    maint.display.seen = true;
    maint.display.lastSeenMs = now;
  }
}

/* The display would send this packet; the BOOT button plays its role locally. */
static void sendLocalAction(uint8_t action, uint8_t argument) {
  GamePacket pkt;
  buildPacket(&pkt, ID_HOST, ID_DISPLAY, CMD_ACTION,
              (uint16_t)(((uint16_t)action << 8) | argument));
  game.onPacket((const uint8_t*)&pkt, PACKET_SIZE, micros());
}

/* BOOT (GPIO0) held 3 s toggles maintenance mode (DESIGN.md 9.1). */
static void handleBootButton() {
  static bool wasDown = false;
  static bool handled = false;
  static uint32_t downSince = 0;

  bool down = digitalRead(PIN_BOOT) == LOW;
  uint32_t now = millis();

  if (down) {
    if (!wasDown) downSince = now;
    if (!handled && (uint32_t)(now - downSince) >= 3000) {
      handled = true;
      if (game.phase() == PH_MAINT) {
        sendLocalAction(ACT_MAINTENANCE, 0);
      } else if (game.phase() == PH_IDLE) {
        sendLocalAction(ACT_MAINTENANCE, 1);
      }
    }
  } else {
    handled = false;
    downSince = 0;
  }
  wasDown = down;
}

/* A CMD_DIAG frame goes to the debug table only, never to the game. */
static bool isDiagFrame(const RxFrame& frame) {
  if (!validatePacket(frame.data, frame.len)) return false;
  return frame.data[3] == CMD_DIAG;
}

/* ---------------------------------------------------------------- service */

/* A CMD_SVC_REQ frame goes to the service handler only, never to the game. */
static bool isSvcFrame(const RxFrame& frame) {
  if (!validatePacket(frame.data, frame.len)) return false;
  return frame.data[3] == CMD_SVC_REQ;
}

/* Reports run while the last request is younger than SVC_HOLD_MS (DESIGN.md 12). */
static uint32_t svcLastReqMs = 0;
static uint32_t svcLastSendMs = 0;
static uint8_t svcSeq = 0;
static bool svcActive = false;

/* Only the display may ask the host. An unknown op is ignored. */
static void handleServiceRequest(const RxFrame& frame) {
  if (frame.data[2] != ID_DISPLAY || frame.data[1] != ID_HOST) return;

  uint32_t now = millis();
  if (frame.data[4] == SVC_OP_DEBUG_WIFI) {
    bool on = frame.data[5] != 0;
    if (on != storage.debugWifi()) writeDebugWifi(on);
  } else if (frame.data[4] != SVC_OP_POLL) {
    return;
  }

  if (!svcActive) {
    svcActive = true;
    svcLastSendMs = now - SVC_SEND_MS;  /* the first report goes out at once */
  }
  svcLastReqMs = now;
}

/* One ServicePacket every SVC_SEND_MS; nothing before the first request. */
static void pumpService() {
  if (!svcActive) return;
  uint32_t now = millis();
  if ((uint32_t)(now - svcLastReqMs) >= SVC_HOLD_MS) return;
  if ((uint32_t)(now - svcLastSendMs) < SVC_SEND_MS) return;
  svcLastSendMs = now;

  ServicePacket sp;
  debug.fillService(sp, now);
  sp.seq = svcSeq++;
  if (storage.debugWifi()) sp.host_flags |= SVH_DEBUG_WIFI;
  if (maint.apActive()) {
    sp.host_flags |= SVH_AP_ON;
    sp.ap_clients = (uint8_t)WiFi.softAPgetStationNum();
  }
  sealService(&sp);
  ports.send(ID_DISPLAY, (const uint8_t*)&sp, SVC_SIZE);
}

/*
 * Everything that must keep running while the maintenance web server streams a
 * large body (DESIGN.md H10: a beacon every 500 ms in every phase). Called from
 * loop() and between the chunks of an upload; the guard stops a pump that runs
 * from inside onPacket() -> maintEnter() from nesting.
 */
static void pumpGame() {
  static bool busy = false;
  if (busy) return;
  busy = true;

  /* Every received frame is handled in this thread (H3); callbacks only queue. */
  RxFrame frame;
  while (radio.pop(frame)) {
    debug.noteRx(frame.data, frame.len, frame.us);
    sniffDisplay(frame);
    if (isDiagFrame(frame)) continue;
    if (isSvcFrame(frame)) {
      handleServiceRequest(frame);
      continue;
    }
    game.onPacket(frame.data, frame.len, frame.us);
  }

  /* Timers, retries, the beacon and the snapshot send (buildState -> send). */
  game.tick();

  /* Service reports keep their rhythm between the chunks of an upload too. */
  pumpService();

  /* The game state only changes in tick(): log each phase and join change once. */
  static bool haveState = false;
  static uint8_t lastPhase = 0;
  static uint8_t lastJoined = 0;
  uint8_t phase = game.phase();
  uint8_t joined = game.state().joined_mask;
  if (!haveState) {
    haveState = true;
    lastPhase = phase;
    lastJoined = joined;
  } else {
    if (phase != lastPhase) {
      debug.notePhase(lastPhase, phase);
      lastPhase = phase;
    }
    if (joined != lastJoined) {
      debug.noteJoined(lastJoined, joined);
      lastJoined = joined;
    }
  }

  busy = false;
}

/* Boot banner (DESIGN.md 11.2): marker, version, reset reason, MAC, heap. */
static void bootBanner() {
  Serial.println();
  Serial.println(FW_MARKER);
  Serial.printf("===== Reaction Time Duel - host %s =====\n", FW_VERSION_STRING);
  Serial.printf("reset: %d (%s)\n", debug.resetReason(), debug.resetReasonText());
  Serial.printf("host MAC: %s\n", WiFi.macAddress().c_str());
  Serial.printf("debug_wifi: %s\n", storage.debugWifi() ? "on" : "off");
  Serial.printf("free heap: %lu\n", (unsigned long)ESP.getFreeHeap());
}

void setup() {
  /* Room for a burst of [EVT] lines; Debug::flushSerial() never blocks. */
  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
  pinMode(PIN_BOOT, INPUT_PULLUP);

  leds.begin();
  audio.begin();
  storage.begin();
  radio.begin();

  uint8_t cfg[CFG_SLOTS];
  uint16_t bestAlltime = TIME_NONE;
  uint16_t gamesTotal = 0;
  storage.load(cfg, bestAlltime, gamesTotal);

  debug.setRadio(&radio);
  debug.setWifiHooks(readDebugWifi, writeDebugWifi);
  debug.begin();
  bootBanner();

  ports.maint = &maint;
  maint.setPump(pumpGame);
  maint.begin();

  randomSeed(esp_random());
  game.begin(cfg, bestAlltime, gamesTotal);

  /* A host update restarted the host from update mode: go back to update mode. */
  if (storage.takeResumeMaint()) {
    Serial.println("[MAINT] update mode resumed after a host update");
    sendLocalAction(ACT_MAINTENANCE, 1);
  }

  Serial.println("Host ready");
}

void loop() {
  uint32_t loopStartUs = micros();

  pumpGame();

  handleBootButton();

  leds.update(game.leds());
  audio.update();
  storage.tick();
  maint.update();
  debug.run();

  debug.noteLoop((uint32_t)(micros() - loopStartUs));
  delay(1);
}
