/*
 * Native tests for the service report frame (RtdService.h, DESIGN.md 12).
 * test_main.cpp registers serviceChecks() as T23.
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "RtdService.h"

static int g_fails = 0;

static void fail(int line, const char* expr) {
  g_fails++;
  printf("  check at %s:%d: %s\n", __FILE__, line, expr);
}

#define CHECK(c) do { if (!(c)) fail(__LINE__, #c); } while (0)

/* One report with every field set to a value that is not zero. */
static void fillPattern(ServicePacket& sp) {
  memset(&sp, 0, sizeof(sp));
  sp.start = SVC_START;
  sp.version = PROTO_VERSION;
  sp.seq = 7;
  sp.host_flags = SVH_DEBUG_WIFI | SVH_AP_ON;
  sp.uptime_min = 1234;
  sp.rx_frames = 4321;
  sp.rx_drop = 3;
  sp.bad_frame = 4;
  sp.tx_fail_bcast = 5;
  sp.heap_kb = 77;
  sp.heap_min_kb = 55;
  sp.loop_max_ms = 9;
  sp.reset_reason = 2;
  sp.ap_clients = 2;
  for (uint8_t i = 0; i < SVC_DEVICES; i++) {
    sp.dev[i].flags = SVD_SEEN | SVD_DIAG;
    sp.dev[i].age_ds = (uint8_t)(i * 10);
    sp.dev[i].frames = (uint16_t)(100 + i);
    sp.dev[i].host_tx_fail = (uint8_t)(i + 1);
    sp.dev[i].fw = 0x40;
    sp.dev[i].reset_reason = 1;
    sp.dev[i].uptime_min = 20;
    sp.dev[i].heap_kb = 30;
    sp.dev[i].rx_drop = 0;
    sp.dev[i].tx_fail = 6;
    sp.dev[i].bad_frame = 7;
    sp.dev[i].loop_max_ms = 8;
    sp.dev[i].link_gap_ds = 9;
    sp.dev[i].extra = 10;
  }
  sealService(&sp);
}

static void testLayout() {
  CHECK(sizeof(ServicePacket) == SVC_SIZE);
  CHECK(sizeof(ServiceDevice) == 15);
  CHECK(offsetof(ServicePacket, version) == 1);
  CHECK(offsetof(ServicePacket, seq) == 2);
  CHECK(offsetof(ServicePacket, host_flags) == 3);
  CHECK(offsetof(ServicePacket, uptime_min) == 4);
  CHECK(offsetof(ServicePacket, rx_frames) == 6);
  CHECK(offsetof(ServicePacket, ap_clients) == 15);
  CHECK(offsetof(ServicePacket, dev) == 16);
  CHECK(offsetof(ServicePacket, crc) == 91);
  CHECK(offsetof(ServiceDevice, flags) == 0);
  CHECK(offsetof(ServiceDevice, age_ds) == 1);
  CHECK(offsetof(ServiceDevice, frames) == 2);
  CHECK(offsetof(ServiceDevice, host_tx_fail) == 4);
  CHECK(offsetof(ServiceDevice, link_gap_ds) == 13);
  CHECK(offsetof(ServiceDevice, extra) == 14);
}

static void testSeal() {
  ServicePacket sp;
  fillPattern(sp);
  CHECK(sp.start == SVC_START);
  CHECK(sp.version == PROTO_VERSION);
  CHECK(sp.crc == calcCRC8((const uint8_t*)&sp, SVC_SIZE - 1));
  CHECK(validateService((const uint8_t*)&sp, SVC_SIZE) == 1);
  CHECK(validateService((const uint8_t*)&sp, PACKET_SIZE) == 0);

  /* A flipped payload byte. */
  uint8_t buf[SVC_SIZE];
  memcpy(buf, &sp, sizeof(buf));
  buf[40] ^= 0x01;
  CHECK(validateService(buf, SVC_SIZE) == 0);

  /* A flipped CRC byte. */
  memcpy(buf, &sp, sizeof(buf));
  buf[SVC_SIZE - 1] ^= 0x01;
  CHECK(validateService(buf, SVC_SIZE) == 0);

  /* Wrong length, in both directions. */
  memcpy(buf, &sp, sizeof(buf));
  CHECK(validateService(buf, SVC_SIZE - 1) == 0);
  CHECK(validateService(buf, SVC_SIZE + 1) == 0);

  /* Wrong start byte. */
  memcpy(buf, &sp, sizeof(buf));
  buf[0] = STATE_START;
  CHECK(validateService(buf, SVC_SIZE) == 0);

  /* Wrong protocol version. */
  memcpy(buf, &sp, sizeof(buf));
  buf[1] = (uint8_t)(PROTO_VERSION + 1);
  CHECK(validateService(buf, SVC_SIZE) == 0);
}

static void testClamp() {
  CHECK(rtdSvcClamp8(0) == 0);
  CHECK(rtdSvcClamp8(255) == 255);
  CHECK(rtdSvcClamp8(256) == 255);
  CHECK(rtdSvcClamp8(100000) == 255);
  CHECK(rtdSvcClamp16(0) == 0);
  CHECK(rtdSvcClamp16(65535) == 65535);
  CHECK(rtdSvcClamp16(65536) == 65535);
  CHECK(rtdSvcClamp16(1000000) == 65535);
}

static void testAge() {
  CHECK(rtdSvcAge(0) == 0);
  CHECK(rtdSvcAge(99) == 0);
  CHECK(rtdSvcAge(100) == 1);
  CHECK(rtdSvcAge(25400) == 254);
  CHECK(rtdSvcAge(25500) == 254);
  CHECK(rtdSvcAge(0xFFFFFFFFu) == 254);
  CHECK(rtdSvcAge(0xFFFFFFFFu) != SVC_AGE_NEVER);
}

int serviceChecks() {
  g_fails = 0;
  testLayout();
  testSeal();
  testClamp();
  testAge();
  return g_fails;
}
