#ifndef RTD_SERVICE_H
#define RTD_SERVICE_H

/*
 * Service menu data (DESIGN.md 12). The display shows the link status and the
 * diagnostics numbers of all devices on its service screen. The numbers live
 * in the host debug hub, so the display asks for them:
 *
 *   display -> host : GamePacket, cmd = CMD_SVC_REQ, dest_id = ID_HOST,
 *                     src_id = ID_DISPLAY, data_high = SVC_OP_*, data_low = argument.
 *   host -> display : one ServicePacket (SVC_SIZE bytes, start byte SVC_START)
 *                     every SVC_SEND_MS while the last request is younger than
 *                     SVC_HOLD_MS.
 *
 * The display sends SVC_OP_POLL every SVC_POLL_MS while the service screen is
 * open, so the host sends nothing when nobody looks. HostGame, StickLogic and
 * DisplayLink ignore both frames; the firmware glue sends and reads them.
 */

#include <stdint.h>
#include "RtdProtocol.h"

#define CMD_SVC_REQ        0x47

#define SVC_OP_POLL        0   /* keep the reports coming; data_low unused */
#define SVC_OP_DEBUG_WIFI  1   /* store debug_wifi = (data_low != 0); effective at the next boot */

#define SVC_POLL_MS        1000
#define SVC_SEND_MS        500
#define SVC_HOLD_MS        3000

#define SVC_START          0x5B
#define SVC_SIZE           92
#define SVC_DEVICES        5      /* index 0..3 = stick ID 1..4, index 4 = display */
#define SVC_AGE_NEVER      0xFF   /* age_ds: no frame since the host booted */

/* ServiceDevice.flags */
#define SVD_SEEN           0x01   /* the host received at least one frame of this device */
#define SVD_DIAG           0x02   /* at least one CMD_DIAG report arrived (the diag fields are valid) */
#define SVD_SENSOR_OK      0x04   /* stick: DIAG_MPU_OK = 1, display: DIAG_TOUCH_OK = 1 */

/* ServicePacket.host_flags */
#define SVH_DEBUG_WIFI     0x01   /* stored debug_wifi setting */
#define SVH_AP_ON          0x02   /* the access point runs now */
#define SVH_TRACE          0x04   /* event trace is on */

/* One row of the device table. Every counter saturates (rtdSvcClamp8 / rtdSvcClamp16). */
typedef struct __attribute__((packed)) {
  uint8_t  flags;         /*  0 SVD_* */
  uint8_t  age_ds;        /*  1 time since the last frame, 0.1 s units, 254 = 25.4 s or more, SVC_AGE_NEVER */
  uint16_t frames;        /*  2 frames received from the device since the host booted */
  uint8_t  host_tx_fail;  /*  4 host sends to this device that failed */
  uint8_t  fw;            /*  5 DIAG_FW_VERSION, 0 = unknown */
  uint8_t  reset_reason;  /*  6 DIAG_RESET_REASON */
  uint8_t  uptime_min;    /*  7 DIAG_UPTIME_MIN */
  uint8_t  heap_kb;       /*  8 DIAG_HEAP_KB */
  uint8_t  rx_drop;       /*  9 DIAG_RX_DROP */
  uint8_t  tx_fail;       /* 10 DIAG_TX_FAIL */
  uint8_t  bad_frame;     /* 11 DIAG_BAD_FRAME */
  uint8_t  loop_max_ms;   /* 12 DIAG_LOOP_MAX_MS */
  uint8_t  link_gap_ds;   /* 13 DIAG_LINK_GAP_DS */
  uint8_t  extra;         /* 14 stick: DIAG_STICK_STATE, display: DIAG_SNAP_PER_10S */
} ServiceDevice;          /* 15 bytes */

typedef struct __attribute__((packed)) {
  uint8_t  start;            /*  0 SVC_START */
  uint8_t  version;          /*  1 PROTO_VERSION */
  uint8_t  seq;              /*  2 +1 per transmission */
  uint8_t  host_flags;       /*  3 SVH_* */
  uint16_t uptime_min;       /*  4 host uptime, minutes */
  uint16_t rx_frames;        /*  6 frames received by the host */
  uint8_t  rx_drop;          /*  8 frames lost because the host receive ring was full */
  uint8_t  bad_frame;        /*  9 frames with a wrong length, start byte or CRC */
  uint8_t  tx_fail_bcast;    /* 10 failed broadcast sends */
  uint8_t  heap_kb;          /* 11 free heap, KB */
  uint8_t  heap_min_kb;      /* 12 lowest free heap since boot, KB */
  uint8_t  loop_max_ms;      /* 13 longest loop pass of the last 10 s, ms */
  uint8_t  reset_reason;     /* 14 esp_reset_reason() of the host */
  uint8_t  ap_clients;       /* 15 stations joined to the access point */
  ServiceDevice dev[SVC_DEVICES];  /* 16..90 */
  uint8_t  crc;              /* 91 CRC8 over bytes 0..90 */
} ServicePacket;

typedef char rtd_svc_device_size_check[(sizeof(ServiceDevice) == 15) ? 1 : -1];
typedef char rtd_svc_packet_size_check[(sizeof(ServicePacket) == SVC_SIZE) ? 1 : -1];

static inline uint8_t rtdSvcClamp8(uint32_t v) { return v > 255u ? 255u : (uint8_t)v; }
static inline uint16_t rtdSvcClamp16(uint32_t v) { return v > 65535u ? 65535u : (uint16_t)v; }

/* Age in 0.1 s units for age_ds (never returns SVC_AGE_NEVER). */
static inline uint8_t rtdSvcAge(uint32_t ms) {
  uint32_t ds = ms / 100u;
  return ds > 254u ? 254u : (uint8_t)ds;
}

static inline void sealService(ServicePacket* sp) {
  sp->start = SVC_START;
  sp->version = PROTO_VERSION;
  sp->crc = calcCRC8((const uint8_t*)sp, SVC_SIZE - 1);
}

static inline int validateService(const uint8_t* data, int len) {
  if (len != SVC_SIZE || data[0] != SVC_START || data[1] != PROTO_VERSION) return 0;
  return calcCRC8(data, SVC_SIZE - 1) == data[SVC_SIZE - 1];
}

#endif /* RTD_SERVICE_H */
