#pragma once

/*
 * Display diagnostics (DESIGN.md 11.1, 11.4): counters, CMD_DIAG reports to the host, numbers for the
 * on-screen overlay. diag_rx_drop() and diag_tx_fail() may be called from the radio callbacks (atomic
 * increment only); every other function runs in the LVGL task.
 */

#include <stdint.h>

/* Radio callbacks: ESP-NOW receive queue was full / send callback reported a failure. */
void diag_rx_drop();
void diag_tx_fail();

/* LVGL task: a STATE_SIZE frame that DisplayLink rejected. */
void diag_bad_frame();
/* LVGL task: a valid snapshot arrived (now_ms = its receive time). */
void diag_snapshot(uint32_t now_ms);
/* LVGL task: duration of one UI timer pass. */
void diag_loop_pass(uint32_t ms);
/* main.cpp: result of touch_gt911_init(). */
void diag_set_touch_ok(bool ok);

/* LVGL task, every UI timer pass: rolls the 10 s window and sends one CMD_DIAG per DIAG_INTERVAL_MS
 * (not while ota_busy()). screen_phase = PH_* phase that the screen shows. */
void diag_tick(uint32_t now_ms, uint8_t screen_phase);

struct DiagStats {
    uint32_t rx_drop, tx_fail, bad_frame;
    uint32_t snap_10s;
    uint32_t loop_peak_ms; /* longest UI timer pass since boot */
    uint32_t heap_kb;      /* internal free heap */
    int reset_reason;      /* esp_reset_reason() */
    bool touch_ok;         /* result of touch_gt911_init() */
};
void diag_stats(DiagStats &s, uint32_t now_ms);
const char *diag_reset_name(int reason);
