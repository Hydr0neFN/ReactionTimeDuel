#ifndef RTD_STORAGE_H
#define RTD_STORAGE_H

/*
 * NVS persistence, namespace "rtd" (DESIGN.md 8, 11.3): the settings array,
 * the all-time record, the games counter and the debug_wifi switch. Flash
 * writes are debounced by 2 s; tick() commits a pending write from the loop.
 */

#include <stdint.h>
#include <Preferences.h>
#include "RtdProtocol.h"

#define STORAGE_DEBOUNCE_MS 2000

class Storage {
public:
  Storage();

  void begin();

  /* Load settings (defaults when empty) and the records (TIME_NONE / 0). */
  void load(uint8_t* cfg, uint16_t& bestAlltimeMs, uint16_t& gamesTotal);

  /* Mark the settings dirty; written after 2 s without a change. */
  void saveSettings(const uint8_t* cfg);

  /* Mark the records dirty; written after 2 s without a change. */
  void saveStats(uint16_t bestAlltimeMs, uint16_t gamesTotal);

  /* Debug Wi-Fi (DESIGN.md 11.3): own key, default on. Written at once. */
  bool debugWifi() const { return debugWifi_; }
  void setDebugWifi(bool on);

  /* Read and clear the "enter update mode at boot" flag (set before a restart after a host update). */
  bool takeResumeMaint();
  void setResumeMaint();

  /* Commit a pending write when the debounce expired. */
  void tick();

  /* Commit now (used before a reboot). */
  void flush();

private:
  void write();

  Preferences prefs_;
  bool open_;

  uint8_t cfg_[CFG_SLOTS];
  uint16_t bestAlltimeMs_;
  uint16_t gamesTotal_;
  bool debugWifi_;

  bool settingsDirty_;
  bool statsDirty_;
  uint32_t changedAtMs_;
};

#endif /* RTD_STORAGE_H */
