#ifndef RTD_MAINT_H
#define RTD_MAINT_H

/*
 * Maintenance mode (DESIGN.md 9) and the debug hub web page (11.2, 11.3):
 * soft AP "RTD-Host", HTTP server, firmware uploads, the joystick image for
 * the sticks, the status page and the debug page. The AP starts on demand, or
 * at boot when the debug_wifi setting is on.
 */

#include <Arduino.h>
#include <WebServer.h>
#include <FS.h>
#include <MD5Builder.h>
#include "HostGame.h"
#include "Storage.h"
#include "Debug.h"

struct DisplayStatus {
  bool     seen;
  uint32_t lastSeenMs;
  uint8_t  ver;        /* version byte from CMD_DISPLAY_HELLO, 0 = unknown */
  uint8_t  patch;
  uint8_t  ipOctet;    /* last IP octet from CMD_DISPLAY_INFO, 0 = unknown */
  uint8_t  otaState;
};

class Maint {
public:
  /* Drains the radio ring and ticks the game (main.cpp). Called between the
   * chunks of a long HTTP body so that the beacon keeps its 500 ms. */
  typedef void (*PumpFn)();

  Maint(HostGame& game, Storage& storage, Debug& debug);

  void setPump(PumpFn fn) { pump_ = fn; }

  /* Read the stored joystick image version (normal mode). */
  void begin();

  /* Start AP + web server (called from HostGame's maintEnter). */
  void enter();

  /* Flush the settings and reboot into normal mode (maintExit). */
  void exit();

  /* Serve requests; call from the main loop, does nothing when inactive. */
  void update();

  bool active() const { return active_; }

  /* The access point runs now: maintenance mode, or debug Wi-Fi (DESIGN.md 12). */
  bool apActive() const { return active_; }

  /* Stored joystick image: presence and version (polled by HostGame). */
  bool stickImage(uint8_t* ver, uint8_t* patch);

  /* Written by the radio glue from CMD_DISPLAY_HELLO / CMD_DISPLAY_INFO. */
  DisplayStatus display;

private:
  /* Start the AP and register the routes; does nothing when already active. */
  void startServer();

  /* True in PH_MAINT; otherwise answers 409 and the caller returns. */
  bool requireMaint();

  void handleRoot();
  void handleStatus();
  void handleDebugPage();
  void handleDebugJson();
  void handleDebugLog();
  void handleDebugTrace();
  void handleDebugWifi();
  void handleJoystickImage();
  void handleHostUpload(HTTPUpload& upload);
  void handleHostUploadDone();
  void handleJoystickUpload(HTTPUpload& upload);
  void handleJoystickUploadDone();
  void handleOtaSticks();
  void handleExit();

  void refreshImageInfo();
  void sendAction(uint8_t action, uint8_t argument);

  void feedMarker(const uint8_t* data, size_t len, bool& found, uint8_t& match,
                  const char* needle, uint8_t needleLen,
                  char* version, uint8_t* versionLen, bool* versionDone);
  bool storeImageVersion(uint8_t ver, uint8_t patch, uint32_t size, const String& md5);
  void clearImageRecord();
  bool imageWrite(const uint8_t* data, size_t len);
  bool imageMd5(uint32_t size, String& md5, uint32_t* bytesRead);
  void pump() { if (pump_ != nullptr) pump_(); }

  HostGame& game_;
  Storage& storage_;
  Debug& debug_;
  WebServer server_;

  PumpFn pump_;

  bool active_;
  bool rebootPending_;
  uint32_t rebootAtMs_;
  bool uploadRejected_;         /* an upload arrived outside PH_MAINT */

  bool imageValid_;
  uint8_t imageVer_;
  uint8_t imagePatch_;
  uint32_t imageSize_;          /* size recorded at the end of the upload */

  String imageMd5_;             /* MD5 of the stored image, checked at boot and after an upload */
  const void* imagePart_;       /* esp_partition_t of the OTA slot that holds the image */
  uint32_t imageErasedTo_;      /* the slot is erased up to this offset in this upload */
  bool hostUploadOk_;
  bool hostUpdateOk_;
  bool hostMarkerFound_;
  uint8_t hostMatch_;
  uint32_t joystickUploadBytes_;  /* bytes received in this upload */
  uint32_t joystickReadback_;     /* bytes that the stored image gave back after the upload */
  MD5Builder joystickMd5_;        /* MD5 of the received bytes */
  bool joystickUploadOk_;
  bool joystickMarkerFound_;
  uint8_t joystickMatch_;
  char joystickVersion_[12];
  uint8_t joystickVersionLen_;
  bool joystickVersionDone_;
};

#endif /* RTD_MAINT_H */
