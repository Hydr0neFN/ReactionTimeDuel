#include "Storage.h"

#include <Arduino.h>
#include <string.h>

static const char* NS_NAME = "rtd";
static const char* KEY_CFG = "cfg";
static const char* KEY_BEST = "best";
static const char* KEY_GAMES = "games";
static const char* KEY_DEBUG_WIFI = "dbgwifi";
static const char* KEY_RESUME_MAINT = "resmaint";

Storage::Storage()
    : open_(false),
      bestAlltimeMs_(TIME_NONE),
      gamesTotal_(0),
      debugWifi_(true),
      settingsDirty_(false),
      statsDirty_(false),
      changedAtMs_(0) {
  memcpy(cfg_, RTD_CFG_DEF, sizeof(cfg_));
  cfg_[0] = 0;
}

void Storage::begin() {
  open_ = prefs_.begin(NS_NAME, false);
  if (!open_) Serial.println("[STORAGE] NVS namespace rtd unavailable");
}

void Storage::load(uint8_t* cfg, uint16_t& bestAlltimeMs, uint16_t& gamesTotal) {
  memcpy(cfg_, RTD_CFG_DEF, sizeof(cfg_));
  cfg_[0] = 0;
  bestAlltimeMs_ = TIME_NONE;
  gamesTotal_ = 0;
  debugWifi_ = true;

  if (open_) {
    if (prefs_.getBytesLength(KEY_CFG) == CFG_SLOTS) {
      prefs_.getBytes(KEY_CFG, cfg_, CFG_SLOTS);
      cfg_[0] = 0;
    }
    if (prefs_.isKey(KEY_BEST)) bestAlltimeMs_ = prefs_.getUShort(KEY_BEST, TIME_NONE);
    if (prefs_.isKey(KEY_GAMES)) gamesTotal_ = prefs_.getUShort(KEY_GAMES, 0);
    if (prefs_.isKey(KEY_DEBUG_WIFI)) debugWifi_ = prefs_.getBool(KEY_DEBUG_WIFI, true);
  }

  memcpy(cfg, cfg_, CFG_SLOTS);
  bestAlltimeMs = bestAlltimeMs_;
  gamesTotal = gamesTotal_;
}

void Storage::saveSettings(const uint8_t* cfg) {
  memcpy(cfg_, cfg, CFG_SLOTS);
  cfg_[0] = 0;
  settingsDirty_ = true;
  changedAtMs_ = millis();
}

void Storage::saveStats(uint16_t bestAlltimeMs, uint16_t gamesTotal) {
  bestAlltimeMs_ = bestAlltimeMs;
  gamesTotal_ = gamesTotal;
  statsDirty_ = true;
  changedAtMs_ = millis();
}

void Storage::setDebugWifi(bool on) {
  debugWifi_ = on;
  if (open_) prefs_.putBool(KEY_DEBUG_WIFI, on);
}

bool Storage::takeResumeMaint() {
  if (!open_ || !prefs_.isKey(KEY_RESUME_MAINT)) return false;
  bool on = prefs_.getBool(KEY_RESUME_MAINT, false);
  prefs_.remove(KEY_RESUME_MAINT);
  return on;
}

void Storage::setResumeMaint() {
  if (open_) prefs_.putBool(KEY_RESUME_MAINT, true);
}

void Storage::tick() {
  if (!settingsDirty_ && !statsDirty_) return;
  if (millis() - changedAtMs_ < STORAGE_DEBOUNCE_MS) return;
  write();
}

void Storage::flush() {
  if (settingsDirty_ || statsDirty_) write();
}

void Storage::write() {
  if (open_) {
    if (settingsDirty_) prefs_.putBytes(KEY_CFG, cfg_, CFG_SLOTS);
    if (statsDirty_) {
      prefs_.putUShort(KEY_BEST, bestAlltimeMs_);
      prefs_.putUShort(KEY_GAMES, gamesTotal_);
    }
  }
  settingsDirty_ = false;
  statsDirty_ = false;
}
