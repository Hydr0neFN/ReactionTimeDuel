#include "diag.h"

#include <atomic>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "net.h"
#include "ota.h"

#include "RtdDiag.h"
#include "RtdProtocol.h"

static std::atomic<uint32_t> s_rx_drop{0};
static std::atomic<uint32_t> s_tx_fail{0};

/* Everything below is touched by the LVGL task only. */
static uint32_t s_bad_frame;
static uint16_t s_bucket[10]; /* valid snapshots per second, last 10 s */
static uint32_t s_bucket_sec;
static bool s_have_snap;
static uint32_t s_last_snap_ms;
static uint32_t s_gap_max_ms;   /* reset after each DIAG_LINK_GAP_DS report */
static uint32_t s_loop_max_ms;  /* reset after each DIAG_LOOP_MAX_MS report */
static uint32_t s_loop_peak_ms; /* since boot, for the overlay */
static bool s_touch_ok;
static uint32_t s_last_send_ms;
static uint8_t s_next_key;

void diag_rx_drop() { s_rx_drop.fetch_add(1, std::memory_order_relaxed); }
void diag_tx_fail() { s_tx_fail.fetch_add(1, std::memory_order_relaxed); }
void diag_bad_frame() { s_bad_frame++; }
void diag_set_touch_ok(bool ok) { s_touch_ok = ok; }

static void roll(uint32_t now_ms)
{
    uint32_t sec = now_ms / 1000;
    uint32_t d = sec - s_bucket_sec;
    if (d == 0) return;
    if (d >= 10) {
        memset(s_bucket, 0, sizeof(s_bucket));
    } else {
        for (uint32_t i = 1; i <= d; i++) s_bucket[(s_bucket_sec + i) % 10] = 0;
    }
    s_bucket_sec = sec;
}

void diag_snapshot(uint32_t now_ms)
{
    roll(now_ms);
    s_bucket[s_bucket_sec % 10]++;
    if (s_have_snap) {
        uint32_t gap = (uint32_t)(now_ms - s_last_snap_ms);
        if (gap > s_gap_max_ms) s_gap_max_ms = gap;
    }
    s_have_snap = true;
    s_last_snap_ms = now_ms;
}

void diag_loop_pass(uint32_t ms)
{
    if (ms > s_loop_max_ms) s_loop_max_ms = ms;
    if (ms > s_loop_peak_ms) s_loop_peak_ms = ms;
}

static uint32_t snap_10s()
{
    uint32_t n = 0;
    for (int i = 0; i < 10; i++) n += s_bucket[i];
    return n;
}

static uint32_t heap_kb() { return (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024); }

/* The gap still running counts too, so a dead link shows a growing value. */
static uint32_t link_gap_ms(uint32_t now_ms)
{
    uint32_t g = s_gap_max_ms;
    if (s_have_snap) {
        uint32_t cur = (uint32_t)(now_ms - s_last_snap_ms);
        if (cur > g) g = cur;
    }
    return g;
}

static const uint8_t kKeys[] = {
    DIAG_FW_VERSION, DIAG_RESET_REASON, DIAG_UPTIME_MIN, DIAG_HEAP_KB,    DIAG_RX_DROP,
    DIAG_TX_FAIL,    DIAG_BAD_FRAME,    DIAG_LOOP_MAX_MS, DIAG_LINK_GAP_DS, DIAG_SNAP_PER_10S,
    DIAG_TOUCH_OK,   DIAG_OTA_STATE,    DIAG_SCREEN,
};

static uint32_t value_of(uint8_t key, uint32_t now_ms, uint8_t screen_phase)
{
    switch (key) {
    case DIAG_FW_VERSION:   return FW_VERSION_BYTE;
    case DIAG_RESET_REASON: return (uint32_t)esp_reset_reason();
    case DIAG_UPTIME_MIN:   return (uint32_t)(esp_timer_get_time() / 60000000LL);
    case DIAG_HEAP_KB:      return heap_kb();
    case DIAG_RX_DROP:      return s_rx_drop.load(std::memory_order_relaxed);
    case DIAG_TX_FAIL:      return s_tx_fail.load(std::memory_order_relaxed);
    case DIAG_BAD_FRAME:    return s_bad_frame;
    case DIAG_LOOP_MAX_MS: {
        uint32_t v = s_loop_max_ms;
        s_loop_max_ms = 0;
        return v;
    }
    case DIAG_LINK_GAP_DS: {
        uint32_t v = link_gap_ms(now_ms) / 100;
        s_gap_max_ms = 0;
        return v;
    }
    case DIAG_SNAP_PER_10S: return snap_10s();
    case DIAG_TOUCH_OK:     return s_touch_ok ? 1 : 0;
    case DIAG_OTA_STATE:    return (uint32_t)ota_state();
    case DIAG_SCREEN:       return screen_phase;
    default:                return 0;
    }
}

void diag_tick(uint32_t now_ms, uint8_t screen_phase)
{
    roll(now_ms);
    if ((uint32_t)(now_ms - s_last_send_ms) < DIAG_INTERVAL_MS || ota_busy()) return;
    s_last_send_ms = now_ms;

    uint8_t key = kKeys[s_next_key];
    s_next_key = (uint8_t)((s_next_key + 1) % (sizeof(kKeys) / sizeof(kKeys[0])));
    uint8_t v = rtdDiagClamp(value_of(key, now_ms, screen_phase));

    GamePacket pkt;
    buildPacket(&pkt, ID_HOST, ID_DISPLAY, CMD_DIAG, (uint16_t)(((uint16_t)key << 8) | v));
    net_ports().send((const uint8_t *)&pkt, PACKET_SIZE);
}

void diag_stats(DiagStats &s, uint32_t now_ms)
{
    roll(now_ms);
    s.rx_drop = s_rx_drop.load(std::memory_order_relaxed);
    s.tx_fail = s_tx_fail.load(std::memory_order_relaxed);
    s.bad_frame = s_bad_frame;
    s.snap_10s = snap_10s();
    s.loop_peak_ms = s_loop_peak_ms;
    s.heap_kb = heap_kb();
    s.reset_reason = (int)esp_reset_reason();
    s.touch_ok = s_touch_ok;
}

const char *diag_reset_name(int reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "?";
    }
}
