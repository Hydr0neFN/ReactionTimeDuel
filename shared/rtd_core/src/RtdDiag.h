#ifndef RTD_DIAG_H
#define RTD_DIAG_H

/*
 * Diagnostics reports (DESIGN.md 11). Sticks and the display send their
 * counters to the host with CMD_DIAG; the host shows them on its debug page
 * and on its serial port. The game logic ignores CMD_DIAG.
 *
 * GamePacket: dest_id = ID_HOST, src_id = the device, cmd = CMD_DIAG,
 * data_high = one DIAG_* key, data_low = the value (saturates at 255).
 * A device sends one key per DIAG_INTERVAL_MS and rotates through its keys.
 * Counters count since boot. "Max" values are reset after each report.
 */

#include <stdint.h>

#define CMD_DIAG              0x46
#define DIAG_INTERVAL_MS      1000
#define DIAG_INTERVAL_FAST_MS 200   /* stick, while the host is in PH_MAINT (service menu) */

/* All devices */
#define DIAG_FW_VERSION       0x01  /* (major << 4) | minor */
#define DIAG_RESET_REASON     0x02  /* platform reset code (rst_info.reason / esp_reset_reason()) */
#define DIAG_UPTIME_MIN       0x03  /* minutes since boot */
#define DIAG_HEAP_KB          0x04  /* free heap in KB */
#define DIAG_RX_DROP          0x05  /* frames dropped because the ring / queue was full */
#define DIAG_TX_FAIL          0x06  /* sends that the send callback reported as failed */
#define DIAG_BAD_FRAME        0x07  /* frames with a wrong length, start byte or CRC */
#define DIAG_LOOP_MAX_MS      0x08  /* longest main loop / UI timer pass since the last report */
#define DIAG_LINK_GAP_DS      0x09  /* longest gap between two host frames since the last report, 0.1 s units */

/* Joysticks */
#define DIAG_BTN_EDGES        0x10  /* button presses seen by the ISR */
#define DIAG_BTN_DROP         0x11  /* presses dropped because the button ring was full */
#define DIAG_STICK_STATE      0x12  /* JS_* state */
#define DIAG_MPU_OK           0x13  /* 1 = the accelerometer answers */

/* Display */
#define DIAG_SNAP_PER_10S     0x20  /* valid snapshots received in the last 10 s */
#define DIAG_TOUCH_OK         0x21  /* 1 = the touch controller answered at boot */
#define DIAG_OTA_STATE        0x22  /* ODS_* state */
#define DIAG_SCREEN           0x23  /* PH_* phase that the screen shows */

static inline uint8_t rtdDiagClamp(uint32_t v) { return (uint8_t)(v > 255 ? 255 : v); }

static inline const char* rtdDiagKeyName(uint8_t key) {
  switch (key) {
    case DIAG_FW_VERSION:   return "fw_version";
    case DIAG_RESET_REASON: return "reset_reason";
    case DIAG_UPTIME_MIN:   return "uptime_min";
    case DIAG_HEAP_KB:      return "heap_kb";
    case DIAG_RX_DROP:      return "rx_drop";
    case DIAG_TX_FAIL:      return "tx_fail";
    case DIAG_BAD_FRAME:    return "bad_frame";
    case DIAG_LOOP_MAX_MS:  return "loop_max_ms";
    case DIAG_LINK_GAP_DS:  return "link_gap_ds";
    case DIAG_BTN_EDGES:    return "btn_edges";
    case DIAG_BTN_DROP:     return "btn_drop";
    case DIAG_STICK_STATE:  return "stick_state";
    case DIAG_MPU_OK:       return "mpu_ok";
    case DIAG_SNAP_PER_10S: return "snap_per_10s";
    case DIAG_TOUCH_OK:     return "touch_ok";
    case DIAG_OTA_STATE:    return "ota_state";
    case DIAG_SCREEN:       return "screen";
    default:                return "?";
  }
}

#endif /* RTD_DIAG_H */
