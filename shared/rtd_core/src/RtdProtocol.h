#ifndef RTD_PROTOCOL_H
#define RTD_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define FW_VERSION_MAJOR  4
#define FW_VERSION_MINOR  0
#define FW_VERSION_PATCH  4
#define FW_VERSION_STRING "V4.0.4"
#define FW_VERSION_BYTE   ((uint8_t)((FW_VERSION_MAJOR << 4) | FW_VERSION_MINOR))
#define PROTO_VERSION     4

/* Hard round limits. The stick reports a penalty at the limit. The host shows
 * the limit on the centre ring and closes the round ROUND_LIMIT_GRACE_MS later. */
#define TIMEOUT_REACTION      3000   /* ms after GO, reaction round */
#define TIMEOUT_SHAKE         15000  /* ms after GO, shake round */
#define ROUND_LIMIT_GRACE_MS  400    /* host wait for the last stick report */

#define RTD_STR2(x) #x
#define RTD_STR(x)  RTD_STR2(x)
/* Marker searched in uploaded images. kind = host | joystick | display (no quotes). */
#define RTD_FW_MARKER(kind) "RTDFW|" #kind "|" RTD_STR(FW_VERSION_MAJOR) "." RTD_STR(FW_VERSION_MINOR) "." RTD_STR(FW_VERSION_PATCH) "|"

#define ESPNOW_CHANNEL    6

/* ---- device IDs ---- */
#define ID_HOST           0x00
#define ID_STICK1         0x01
#define ID_STICK2         0x02
#define ID_STICK3         0x03
#define ID_STICK4         0x04
#define ID_DISPLAY        0x10
#define ID_BROADCAST      0xFF
#define RTD_MAX_PLAYERS   4

/* ---- GamePacket ---- */
#define PACKET_START      0x0A
#define PACKET_SIZE       7

typedef struct __attribute__((packed)) {
  uint8_t start;
  uint8_t dest_id;
  uint8_t src_id;
  uint8_t cmd;
  uint8_t data_high;
  uint8_t data_low;
  uint8_t crc;
} GamePacket;

#define CMD_OK             0x0B
#define CMD_REQ_ID         0x0D
#define CMD_ACK            0x0E
#define CMD_GAME_START     0x21
#define CMD_GO             0x22
#define CMD_VIBRATE        0x23
#define CMD_IDLE           0x24
#define CMD_COUNTDOWN      0x25
#define CMD_REACTION_DONE  0x26
#define CMD_SHAKE_DONE     0x27
#define CMD_SHAKE_PROGRESS 0x28
#define CMD_BEACON         0x29
#define CMD_HEARTBEAT      0x2A
#define CMD_CFG_SET        0x40
#define CMD_ACTION         0x41
#define CMD_DISPLAY_HELLO  0x42
#define CMD_OTA_BEGIN      0x43
#define CMD_OTA_STATUS     0x44
#define CMD_DISPLAY_INFO   0x45

#define MODE_NONE         0x00
#define MODE_REACTION     0x01
#define MODE_SHAKE        0x02

#define TIME_PENALTY      0xFFFF

/* ---- phases ---- */
#define PH_IDLE        0
#define PH_JOIN        1
#define PH_ANNOUNCE    2
#define PH_COUNTDOWN   3
#define PH_REACT_WAIT  4
#define PH_GO          5
#define PH_SHAKE       6
#define PH_RESULTS     7
#define PH_FINAL       8
#define PH_MAINT       9
#define PH_COUNT       10

/* ---- player status ---- */
#define PS_NONE     0   /* not in this round */
#define PS_PLAYING  1   /* in this round, no result yet */
#define PS_DONE     2   /* valid time in time_ms */
#define PS_PENALTY  3   /* false start or timeout */

/* ---- state flags ---- */
#define STF_DEUCE            0x01
#define STF_SCORES           0x02   /* results: winner and scores are visible */
#define STF_NEW_RECORD       0x04
#define STF_CAN_START        0x08   /* join phase with 2 or more players */
#define STF_UPDATE_AVAILABLE 0x10
#define STF_BONUS            0x20   /* bonus round: the points of this round count double */

/* ---- settings ---- */
#define CFG_VOLUME        1
#define CFG_RING_BRIGHT   2
#define CFG_STRIP_BRIGHT  3
#define CFG_ROUNDS        4
#define CFG_GAME_MODE     5
#define CFG_VOICE_GUIDE   6
#define CFG_JOIN_TIME     7
#define CFG_STRIP_MODE    8
#define CFG_DEUCE_LEAD    9
#define CFG_SCORING       10   /* SCORING_* */
#define CFG_BONUS         11   /* BONUS_* */
#define CFG_COUNT         11   /* highest key */
#define CFG_SLOTS         12   /* array size, index = key, index 0 unused */

static const uint8_t RTD_CFG_MIN[CFG_SLOTS] = {0, 0, 1, 0, 1, 0, 0, 3, 0, 1, 0, 0};
static const uint8_t RTD_CFG_MAX[CFG_SLOTS] = {0, 10, 10, 10, 15, 2, 1, 10, 7, 3, 1, 2};
static const uint8_t RTD_CFG_DEF[CFG_SLOTS] = {0, 10, 3, 5, 5, 0, 1, 5, 0, 3, 1, 1};

#define SCORING_CLASSIC   0    /* the round winner gets 1 point */
#define SCORING_PLACES    1    /* a player with a valid time gets 1 point per active player that it beat */
#define BONUS_OFF         0
#define BONUS_LAST        1    /* the last regular round is a bonus round */
#define BONUS_LAST_TWO    2    /* the last two regular rounds are bonus rounds */

/* ---- actions ---- */
#define ACT_ABORT_GAME    1
#define ACT_TEST_SOUND    2
#define ACT_RESET_BEST    3
#define ACT_FACTORY_RESET 4
#define ACT_START_NOW     5
#define ACT_IDENTIFY      6
#define ACT_MAINTENANCE   7
#define ACT_OTA_STICKS    8

/* ---- OTA ---- */
#define OTA_ST_STARTING   1
#define OTA_ST_UPTODATE   2
#define OTA_ST_FAILED     3

#define OTA_NONE          0   /* snapshot ota_state */
#define OTA_READY         1   /* maintenance mode, nothing running */
#define OTA_STICKS        2   /* updating stick ota_target */
#define OTA_DONE          3
#define OTA_ERROR         4   /* at least one stick failed */

/* ---- StatePacket (host -> display) ---- */
#define STATE_START       0x5A
#define STATE_SIZE        72
#define TIME_NONE         0xFFFF

typedef struct __attribute__((packed)) {
  uint8_t  stick_id;     /* 0 = slot empty, else ID_STICK1..4 */
  uint8_t  score;
  uint8_t  status;       /* PS_* */
  uint8_t  progress;     /* shake progress 0..100 */
  uint16_t time_ms;      /* valid when status == PS_DONE, else TIME_NONE */
} PlayerState;           /* 6 bytes */

typedef struct __attribute__((packed)) {
  uint8_t  start;            /*  0 STATE_START */
  uint8_t  version;          /*  1 PROTO_VERSION */
  uint8_t  seq;              /*  2 +1 per transmission */
  uint8_t  phase;            /*  3 PH_* */
  uint8_t  mode;             /*  4 MODE_* */
  uint8_t  round;            /*  5 1-based, 0 before the first round */
  uint8_t  total_rounds;     /*  6 */
  uint8_t  flags;            /*  7 STF_* */
  uint8_t  countdown;        /*  8 3,2,1 in PH_COUNTDOWN, else 0 */
  uint8_t  shake_target;     /*  9 10/15/20 in a shake round, else 0 */
  uint8_t  prompt_slot;      /* 10 1..4 in PH_JOIN, else 0 */
  uint8_t  winner;           /* 11 1..4 round winner (with STF_SCORES) or final winner, else 0 */
  uint8_t  joined_mask;      /* 12 bit0 = player slot 1 */
  uint8_t  active_mask;      /* 13 players in the current round */
  uint8_t  sticks_online;    /* 14 bit0 = stick ID 1 */
  uint8_t  secs_left;        /* 15 PH_SHAKE: seconds left, else 0 */
  PlayerState players[RTD_MAX_PLAYERS]; /* 16..39 */
  uint8_t  cfg[CFG_SLOTS];   /* 40..51 */
  uint16_t best_session_ms;  /* 52 TIME_NONE = none */
  uint16_t best_alltime_ms;  /* 54 */
  uint16_t games_total;      /* 56 */
  uint8_t  host_fw;          /* 58 version byte */
  uint8_t  host_fw_patch;    /* 59 */
  uint8_t  stick_fw[RTD_MAX_PLAYERS];        /* 60 version byte per stick ID, 0 = unknown */
  uint8_t  stick_fw_patch[RTD_MAX_PLAYERS];  /* 64 */
  uint8_t  ota_state;        /* 68 OTA_* */
  uint8_t  ota_target;       /* 69 stick ID being updated, else 0 */
  uint8_t  ota_progress;     /* 70 0..100 */
  uint8_t  crc;              /* 71 CRC8 over bytes 0..70 */
} StatePacket;

/* ---- helpers ---- */
static inline uint8_t calcCRC8(const uint8_t* data, uint8_t len) {
  uint8_t crc = 0x00;
  while (len--) {
    uint8_t extract = *data++;
    for (uint8_t i = 8; i; i--) {
      uint8_t sum = (uint8_t)((crc ^ extract) & 0x01);
      crc >>= 1;
      if (sum) crc ^= 0x8C;
      extract >>= 1;
    }
  }
  return crc;
}

static inline uint16_t packetData(const GamePacket* pkt) {
  return (uint16_t)(((uint16_t)pkt->data_high << 8) | pkt->data_low);
}

static inline void buildPacket(GamePacket* pkt, uint8_t dest, uint8_t src, uint8_t cmd, uint16_t data) {
  pkt->start = PACKET_START;
  pkt->dest_id = dest;
  pkt->src_id = src;
  pkt->cmd = cmd;
  pkt->data_high = (uint8_t)((data >> 8) & 0xFF);
  pkt->data_low = (uint8_t)(data & 0xFF);
  pkt->crc = calcCRC8((const uint8_t*)pkt, PACKET_SIZE - 1);
}

static inline int validatePacket(const uint8_t* data, int len) {
  if (len != PACKET_SIZE || data[0] != PACKET_START) return 0;
  return calcCRC8(data, PACKET_SIZE - 1) == data[PACKET_SIZE - 1];
}

static inline void sealState(StatePacket* st) {
  st->start = STATE_START;
  st->version = PROTO_VERSION;
  st->crc = calcCRC8((const uint8_t*)st, STATE_SIZE - 1);
}

static inline int validateState(const uint8_t* data, int len) {
  if (len != STATE_SIZE || data[0] != STATE_START || data[1] != PROTO_VERSION) return 0;
  return calcCRC8(data, STATE_SIZE - 1) == data[STATE_SIZE - 1];
}

static inline uint8_t cfgClamp(uint8_t key, uint8_t value) {
  if (key < 1 || key > CFG_COUNT) return value;
  if (value < RTD_CFG_MIN[key]) return RTD_CFG_MIN[key];
  if (value > RTD_CFG_MAX[key]) return RTD_CFG_MAX[key];
  return value;
}

#endif /* RTD_PROTOCOL_H */
