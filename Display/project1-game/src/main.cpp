#include "lvgl.h"
#include "display.h"
#include "waveshare_rgb_lcd_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "ui.h"
#include "Protocol.h"
#include "lvgl_port.h"
#include <string.h>

static const char* kTag = "DISPLAY";
static constexpr bool kEnableEspNow = true;
static portMUX_TYPE s_ui_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_pkt_pending = false;
static uint8_t s_pending_pkt[PACKET_SIZE] = {};
static int64_t s_last_ui_update_us = 0;
static uint16_t s_player_time_ms[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
static int16_t s_player_score[4] = {-1, -1, -1, -1};
static bool s_show_scores = false;
static bool s_applied_show_scores = false;
static bool s_has_last_cmd = false;
static uint8_t s_last_cmd = 0;
static uint8_t s_last_data_high = 0;
static uint8_t s_last_data_low = 0;
static bool s_state_dirty = false;
static constexpr int64_t kUiApplyIntervalUs = 150000; // 150ms
static uint8_t s_ready_mask = 0;
static bool s_ready_mask_dirty = false;
static uint8_t s_prompt_mask = 0;
static bool s_prompt_mask_dirty = false;
static uint8_t s_prompt_slot = 0;
static uint8_t s_shake_number = 0;
static bool s_show_deuce = false;
static bool s_applied_show_deuce = false;

// Match panel background color (from UI): 0x101418
static constexpr uint32_t kBorderDefault = 0x101418;

static lv_obj_t* panel_for_player(uint8_t player) {
    switch (player) {
        case 1: return ui_Panel1;
        case 2: return ui_Panel2;
        case 3: return ui_Panel3;
        case 4: return ui_Panel4;
        default: return nullptr;
    }
}

static uint32_t stick_color(uint8_t player) {
    switch (player) {
        case 1: return 0xFFFFFF; // White
        case 2: return 0x0000FF; // Blue
        case 3: return 0xFF0000; // Red
        case 4: return 0xFFFF00; // Yellow
        default: return kBorderDefault;
    }
}

static void set_panel_border_color(uint8_t player, uint32_t color) {
    lv_obj_t* panel = panel_for_player(player);
    if (!panel) {
        return;
    }
    lv_obj_set_style_border_color(panel, lv_color_hex(color), LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void reset_panel_borders(void) {
    for (uint8_t i = 1; i <= 4; i++) {
        set_panel_border_color(i, kBorderDefault);
    }
}


enum class ScreenMode : uint8_t {
    NONE,
    IDLE,
    PROMPT,
    COUNTDOWN,
    GO,
    REACTION,
    SHAKE,
    WINNER,
};

struct UiState {
    ScreenMode mode;
    uint8_t countdown;
    uint8_t winner;
    bool ready[4];
    uint16_t time_ms[4];
    int16_t score[4];
};

static UiState s_pending_state;
static UiState s_applied_state;

static constexpr uint8_t kEspnowChannel = ESPNOW_CHANNEL;
static const uint8_t kHostMac[6] = {0x88, 0x57, 0x21, 0xB3, 0x05, 0xAC};

static void send_ack(uint8_t acked_cmd) {
    GamePacket pkt;
    buildPacket(&pkt, ID_HOST, ID_DISPLAY, CMD_ACK, (uint16_t)acked_cmd);
    esp_err_t err = esp_now_send(kHostMac, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
    if (err == ESP_OK) {
        ESP_LOGI(kTag, "ACK sent for cmd=0x%02X", acked_cmd);
    } else {
        ESP_LOGW(kTag, "ACK send failed for cmd=0x%02X err=%d", acked_cmd, (int)err);
    }
}

static inline void safe_flag(lv_obj_t* obj, bool hide) {
    if (!obj) {
        return;
    }
    if (hide) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_idle() {
    safe_flag(ui_labelCountDown, true);
    safe_flag(ui_labelPlayer1Timing, true);
    safe_flag(ui_labelPlayer2Timing, true);
    safe_flag(ui_labelPlayer3Timing, true);
    safe_flag(ui_labelPlayer4Timing, true);
    safe_flag(ui_imgGo, true);
    safe_flag(ui_imgStart, false);
    safe_flag(ui_centerCircle, false);
    safe_flag(ui_shakeNumber, true);
    safe_flag(ui_imgReactMode, true);
    safe_flag(ui_imgShakeMode, true);
    safe_flag(ui_imgWinner, true);
    safe_flag(ui_labelWinnerNum, true);
    safe_flag(ui_imgDeuce, true);
    reset_panel_borders();
}

static void show_countdown(uint8_t num) {
    safe_flag(ui_centerCircle, false);
    if (ui_labelCountDown) {
        lv_label_set_text_fmt(ui_labelCountDown, "%u", (unsigned)num);
        safe_flag(ui_labelCountDown, false);
    }
    safe_flag(ui_shakeNumber, true);
    safe_flag(ui_imgReactMode, true);
    safe_flag(ui_imgShakeMode, true);
    safe_flag(ui_imgDeuce, true);
    // lv_obj_add_flag(ui_labelPlayMode, LV_OBJ_FLAG_HIDDEN);
    // lv_obj_add_flag(ui_imgWinner, LV_OBJ_FLAG_HIDDEN);
    // lv_obj_add_flag(ui_labelWinnerNum, LV_OBJ_FLAG_HIDDEN);
}

static void show_go() {
    safe_flag(ui_imgStart, true);
    safe_flag(ui_centerCircle, false);
    safe_flag(ui_imgGo, false);
    safe_flag(ui_labelCountDown, true);
    safe_flag(ui_shakeNumber, true);
    safe_flag(ui_imgReactMode, true);
    safe_flag(ui_imgShakeMode, true);
    safe_flag(ui_imgWinner, true);
    safe_flag(ui_labelWinnerNum, true);
    safe_flag(ui_imgDeuce, true);
}

static void show_mode_banner() {
    safe_flag(ui_imgGo, true);
    safe_flag(ui_imgStart, true);
    safe_flag(ui_centerCircle, false);
    safe_flag(ui_labelCountDown, true);
    safe_flag(ui_shakeNumber, true);
    safe_flag(ui_imgReactMode, true);
    safe_flag(ui_imgShakeMode, true);
    safe_flag(ui_imgWinner, true);
    safe_flag(ui_labelWinnerNum, true);
    safe_flag(ui_imgDeuce, true);
}

static lv_obj_t* player_time_label(uint8_t player) {
    switch (player) {
        case 1: return ui_labelPlayer1Timing;
        case 2: return ui_labelPlayer2Timing;
        case 3: return ui_labelPlayer3Timing;
        case 4: return ui_labelPlayer4Timing;
        default: return nullptr;
    }
}

static void update_player_label(uint8_t player) {
    lv_obj_t* label = player_time_label(player);
    if (!label) {
        return;
    }
    uint16_t time_ms = s_player_time_ms[player - 1];
    int16_t score = s_player_score[player - 1];
    char buf[32];
    if (time_ms == 0xFFFF) {
        if (score >= 0) {
            snprintf(buf, sizeof(buf), "W: %u", (unsigned)score);
            lv_label_set_text(label, buf);
            lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (score >= 0) {
        snprintf(buf, sizeof(buf), "%u ms\nW: %u", (unsigned)time_ms, (unsigned)score);
    } else {
        snprintf(buf, sizeof(buf), "%u ms", (unsigned)time_ms);
    }
    lv_label_set_text(label, buf);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
}

static void set_player_time(uint8_t player, uint16_t time_ms) {
    if (player < 1 || player > 4) {
        return;
    }
    s_player_time_ms[player - 1] = time_ms;
    // Remove GO and ring when results are shown
    lv_obj_add_flag(ui_imgGo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_centerCircle, LV_OBJ_FLAG_HIDDEN);
    update_player_label(player);
}

static void clear_time_labels() {
    lv_obj_add_flag(ui_labelPlayer1Timing, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_labelPlayer2Timing, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_labelPlayer3Timing, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_labelPlayer4Timing, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 4; i++) {
        s_player_time_ms[i] = 0xFFFF;
        s_player_score[i] = -1;
    }
}

static void show_winner(uint8_t player) {
    if (player == 0 || player > 4) {
        safe_flag(ui_imgWinner, true);
        safe_flag(ui_labelWinnerNum, true);
        return;
    }
    safe_flag(ui_imgGo, true);
    safe_flag(ui_imgStart, true);
    safe_flag(ui_centerCircle, true);
    safe_flag(ui_labelCountDown, true);
    safe_flag(ui_shakeNumber, true);
    safe_flag(ui_imgReactMode, true);
    safe_flag(ui_imgShakeMode, true);
    safe_flag(ui_imgWinner, false);
    safe_flag(ui_labelWinnerNum, false);
    safe_flag(ui_imgDeuce, true);
    if (ui_labelWinnerNum) {
        lv_label_set_text_fmt(ui_labelWinnerNum, "%u", (unsigned)player);
    }
}

static void update_state_from_packet(const uint8_t *pkt) {
    uint8_t cmd = pkt[3];
    uint8_t data_high = pkt[4];
    uint8_t data_low = pkt[5];
    uint16_t data = ((uint16_t)data_high << 8) | data_low;

    // Only handle display commands; ignore joystick/other commands.
    if (cmd < DISP_IDLE || cmd > DISP_PLAYER_PROMPT) {
        return;
    }

    if (cmd != DISP_DEUCE) {
        s_show_deuce = false;
    }

    if (s_has_last_cmd &&
        cmd == s_last_cmd &&
        data_high == s_last_data_high &&
        data_low == s_last_data_low) {
        return; // skip duplicate updates to reduce flicker
    }
    s_has_last_cmd = true;
    s_last_cmd = cmd;
    s_last_data_high = data_high;
    s_last_data_low = data_low;

    switch (cmd) {
        case DISP_IDLE:
            s_pending_state.mode = ScreenMode::IDLE;
            s_pending_state.countdown = 0xFF;
            s_pending_state.winner = 0;
            s_prompt_mask = 0;
            s_prompt_mask_dirty = true;
            s_show_scores = false;
            for (int i = 0; i < 4; i++) {
                s_pending_state.ready[i] = false;
                s_pending_state.time_ms[i] = 0xFFFF;
                s_pending_state.score[i] = -1;
            }
            break;
        case DISP_PROMPT_JOIN:
            s_pending_state.mode = ScreenMode::PROMPT;
            break;
        case DISP_PLAYER_READY: {
            uint8_t player = 0;
            // New protocol: data_high = player slot (1-4)
            if (data_high >= 1 && data_high <= 4) {
                player = data_high;
            } else if (s_prompt_slot >= 1 && s_prompt_slot <= 4 &&
                       !s_pending_state.ready[s_prompt_slot - 1]) {
                // Fallback to current prompt slot if host doesn't send slot.
                player = s_prompt_slot;
            } else if (data_low >= 1 && data_low <= 4) {
                // Last-resort fallback.
                player = data_low;
            }
            if (player >= 1 && player <= 4) {
                // New protocol: data_low = joystick ID (1-4)
                const uint8_t stick_id = (data_low >= 1 && data_low <= 4) ? data_low : player;
                const uint32_t color = stick_color(stick_id);
                ESP_LOGI(kTag, "DISP_PLAYER_READY slot=%u stick=%u color=0x%06X (raw h=%u l=%u)",
                         player, stick_id, (unsigned)color, data_high, data_low);
                s_pending_state.ready[player - 1] = true;
                s_prompt_mask &= ~(1u << (player - 1));
                s_prompt_mask_dirty = true;
                if (s_prompt_slot == player) {
                    s_prompt_slot = 0;
                }
                s_applied_state.ready[player - 1] = true;
                set_panel_border_color(player, color);
                if (s_pending_state.mode == ScreenMode::NONE ||
                    s_pending_state.mode == ScreenMode::IDLE) {
                    s_pending_state.mode = ScreenMode::PROMPT;
                }
            }
            break;
        }
        case DISP_PLAYER_PROMPT:
            if (data_low >= 1 && data_low <= 4) {
                const uint8_t player = data_low;
                ESP_LOGI(kTag, "DISP_PLAYER_PROMPT slot=%u (raw h=%u l=%u)",
                         player, data_high, data_low);
                // Blink only the currently prompted player.
                s_prompt_mask = (1u << (player - 1));
                s_prompt_mask_dirty = true;
                s_prompt_slot = player;
                s_pending_state.mode = ScreenMode::PROMPT;
            }
            break;
        case DISP_COUNTDOWN:
            if (data_low == 0) {
                // s_pending_state.mode = ScreenMode::GO;
                s_pending_state.countdown = 0;
            } else {
                s_pending_state.mode = ScreenMode::COUNTDOWN;
                s_pending_state.countdown = data_low;
            }
            break;
        case DISP_GO:
            s_pending_state.mode = ScreenMode::GO;
            break;
        case DISP_REACTION_MODE:
            s_pending_state.mode = ScreenMode::REACTION;
            s_prompt_mask = 0;
            s_prompt_mask_dirty = true;
            s_pending_state.winner = 0;
            s_show_scores = false;
            for (int i = 0; i < 4; i++) {
                s_pending_state.time_ms[i] = 0xFFFF;
                s_pending_state.score[i] = -1;
            }
            break;
        case DISP_SHAKE_MODE:
            s_pending_state.mode = ScreenMode::SHAKE;
            s_prompt_mask = 0;
            s_prompt_mask_dirty = true;
            s_pending_state.winner = 0;
            s_show_scores = false;
            s_shake_number = data_low;
            for (int i = 0; i < 4; i++) {
                s_pending_state.time_ms[i] = 0xFFFF;
                s_pending_state.score[i] = -1;
            }
            break;
        case DISP_DEUCE:
            s_show_deuce = true;
            break;
        case DISP_TIME_P1:
            s_pending_state.time_ms[0] = data;
            break;
        case DISP_TIME_P2:
            s_pending_state.time_ms[1] = data;
            break;
        case DISP_TIME_P3:
            s_pending_state.time_ms[2] = data;
            break;
        case DISP_TIME_P4:
            s_pending_state.time_ms[3] = data;
            break;
        case DISP_ROUND_WINNER:
            s_pending_state.mode = ScreenMode::WINNER;
            s_pending_state.winner = data_low;
            break;
        case DISP_SCORES:
            if (data_high >= 1 && data_high <= 4) {
                s_pending_state.score[data_high - 1] = data_low;
            }
            s_show_scores = true;
            break;
        case DISP_FINAL_WINNER:
            s_pending_state.mode = ScreenMode::WINNER;
            s_pending_state.winner = data_low;
            break;
        default:
            (void)data;
            break;
    }
    s_state_dirty = true;
}

static void apply_state(void) {
    // Apply mode changes
    if (s_pending_state.mode != s_applied_state.mode) {
        switch (s_pending_state.mode) {
            case ScreenMode::IDLE:
                show_idle();
                clear_time_labels();
                show_winner(0);
                break;
            case ScreenMode::PROMPT:
                show_idle();
                clear_time_labels();
                show_winner(0);
                break;
            case ScreenMode::COUNTDOWN:
                show_countdown(s_pending_state.countdown);
                break;
            case ScreenMode::GO:
                show_go();
                break;
            case ScreenMode::REACTION:
                show_mode_banner();
                safe_flag(ui_imgReactMode, false);
                safe_flag(ui_imgShakeMode, true);
                clear_time_labels();
                show_winner(0);
                break;
            case ScreenMode::SHAKE:
                show_mode_banner();
                safe_flag(ui_imgReactMode, true);
                safe_flag(ui_imgShakeMode, false);
                if (ui_shakeNumber) {
                    lv_label_set_text_fmt(ui_shakeNumber, "%u", (unsigned)s_shake_number);
                }
                safe_flag(ui_shakeNumber, false);
                clear_time_labels();
                show_winner(0);
                break;
            case ScreenMode::WINNER:
                show_winner(s_pending_state.winner);
                break;
            default:
                break;
        }
        s_applied_state.mode = s_pending_state.mode;
    }

    if (s_pending_state.mode == ScreenMode::COUNTDOWN &&
        s_pending_state.countdown != s_applied_state.countdown) {
        show_countdown(s_pending_state.countdown);
        s_applied_state.countdown = s_pending_state.countdown;
    }

    if (s_pending_state.mode == ScreenMode::WINNER &&
        s_pending_state.winner != s_applied_state.winner) {
        show_winner(s_pending_state.winner);
        s_applied_state.winner = s_pending_state.winner;
    }

    if (s_show_deuce != s_applied_show_deuce) {
        safe_flag(ui_imgDeuce, !s_show_deuce);
        s_applied_show_deuce = s_show_deuce;
    }

    if (s_show_scores && !s_applied_show_scores) {
        for (int i = 0; i < 4; i++) {
            update_player_label(i + 1);
        }
        s_applied_show_scores = true;
    }

    for (int i = 0; i < 4; i++) {
        if (s_pending_state.ready[i] != s_applied_state.ready[i]) {
            s_applied_state.ready[i] = s_pending_state.ready[i];
            if (s_pending_state.ready[i]) {
                // Keep current color (set on ready) if already assigned.
            } else {
                set_panel_border_color(i + 1, kBorderDefault);
            }
        }
    }

    for (int i = 0; i < 4; i++) {
        if (s_pending_state.time_ms[i] != s_player_time_ms[i]) {
            s_player_time_ms[i] = s_pending_state.time_ms[i];
            if (s_show_scores) {
                if (s_pending_state.time_ms[i] != 0xFFFF) {
                    set_player_time(i + 1, s_pending_state.time_ms[i]);
                } else {
                    update_player_label(i + 1);
                }
            }
        }
        if (s_pending_state.score[i] != s_player_score[i]) {
            s_player_score[i] = s_pending_state.score[i];
            if (s_show_scores) {
                update_player_label(i + 1);
            }
        }
    }

    s_applied_state = s_pending_state;
}

static void ui_timer_cb(lv_timer_t *t) {
    (void)t;
    uint8_t pkt[PACKET_SIZE] = {};
    bool has_pkt = false;
    portENTER_CRITICAL(&s_ui_mux);
    if (s_pkt_pending) {
        memcpy(pkt, s_pending_pkt, PACKET_SIZE);
        s_pkt_pending = false;
        has_pkt = true;
    }
    portEXIT_CRITICAL(&s_ui_mux);

    int64_t now_us = esp_timer_get_time();
    if (has_pkt) {
        update_state_from_packet(pkt);
    }
    if (s_state_dirty && (now_us - s_last_ui_update_us >= kUiApplyIntervalUs)) {
        s_last_ui_update_us = now_us;
        apply_state();
        s_state_dirty = false;
    }
    if (s_ready_mask_dirty) {
        for (int i = 0; i < 4; i++) {
            bool on = (s_ready_mask >> i) & 0x1;
            s_pending_state.ready[i] = on;
            s_applied_state.ready[i] = on;
            if (on) {
                // Keep current color (set on ready) if already assigned.
            } else {
                set_panel_border_color(i + 1, kBorderDefault);
            }
        }
        s_ready_mask_dirty = false;
    }

    if (s_prompt_mask_dirty || s_prompt_mask) {
        static uint8_t blink_phase = 0;
        blink_phase = (blink_phase + 1) % 10; // 0..9
        bool blink_on = (blink_phase < 5);

        uint8_t mask = s_prompt_mask;
        for (int i = 0; i < 4; i++) {
            bool prompted = (mask >> i) & 0x1;
            if (prompted && !s_pending_state.ready[i]) {
                set_panel_border_color(i + 1, blink_on ? 0x00FF00 : kBorderDefault);
            }
        }
        s_prompt_mask_dirty = false;
    }
}

static void on_data_recv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len != PACKET_SIZE || !data) {
        ESP_LOGW(kTag, "ESPNOW drop: bad len or null data");
        return;
    }
    if (data[0] != PACKET_START) {
        ESP_LOGW(kTag, "ESPNOW drop: bad start 0x%02X", data[0]);
        return;
    }
    uint8_t dest = data[1];
    if (dest != ID_DISPLAY && dest != ID_BROADCAST) {
        ESP_LOGW(kTag, "ESPNOW drop: wrong dest 0x%02X", dest);
        return;
    }
    if (data[2] != ID_HOST) {
        ESP_LOGW(kTag, "ESPNOW drop: wrong src id 0x%02X", data[2]);
        return;
    }
    if (info && memcmp(info->src_addr, kHostMac, 6) != 0) {
        ESP_LOGW(kTag, "ESPNOW drop: unexpected MAC");
        return;
    }
    uint8_t crc = calcCRC8(data, 6);
    if (crc != data[6]) {
        ESP_LOGW(kTag, "ESPNOW drop: crc mismatch calc=0x%02X pkt=0x%02X", crc, data[6]);
        return;
    }

    if (info) {
        ESP_LOGI(kTag, "ESPNOW rx len=%d src=%02X:%02X:%02X:%02X:%02X:%02X cmd=0x%02X data=%u:%u",
                 len,
                 info->src_addr[0], info->src_addr[1], info->src_addr[2],
                 info->src_addr[3], info->src_addr[4], info->src_addr[5],
                 data[3], data[4], data[5]);
    } else {
        ESP_LOGI(kTag, "ESPNOW rx len=%d src=unknown cmd=0x%02X data=%u:%u",
                 len, data[3], data[4], data[5]);
    }

    // Send ACK for commands that the host retries.
    uint8_t cmd = data[3];
    if (cmd >= DISP_IDLE && cmd <= DISP_PLAYER_PROMPT) {
        if (!(cmd >= DISP_TIME_P1 && cmd <= DISP_TIME_P4) && cmd != DISP_SCORES) {
            send_ack(cmd);
        }
    }
    // Fast-path time updates so they aren't lost when multiple packets arrive quickly.
    if (data[3] >= DISP_TIME_P1 && data[3] <= DISP_TIME_P4) {
        const uint8_t idx = data[3] - DISP_TIME_P1;
        const uint16_t time_ms = ((uint16_t)data[4] << 8) | data[5];
        portENTER_CRITICAL(&s_ui_mux);
        s_pending_state.time_ms[idx] = time_ms;
        s_state_dirty = true;
        portEXIT_CRITICAL(&s_ui_mux);
        ESP_LOGI(kTag, "FAST TIME p%u=%u", (unsigned)(idx + 1), (unsigned)time_ms);
        return;
    }
    if (data[3] == DISP_SCORES && data[4] >= 1 && data[4] <= 4) {
        const uint8_t idx = data[4] - 1;
        portENTER_CRITICAL(&s_ui_mux);
        s_pending_state.score[idx] = data[5];
        s_show_scores = true;
        s_state_dirty = true;
        portEXIT_CRITICAL(&s_ui_mux);
        ESP_LOGI(kTag, "FAST SCORE p%u=%u", (unsigned)(idx + 1), (unsigned)data[5]);
        return;
    }

    // Fast-path DISP_PLAYER_READY so it isn't lost when multiple packets arrive quickly.
    if (data[3] == DISP_PLAYER_READY) {
        uint8_t player = 0;
        if (data[4] >= 1 && data[4] <= 4) {
            // New protocol: data_high = player slot
            player = data[4];
        } else if (s_prompt_slot >= 1 && s_prompt_slot <= 4 &&
                   !s_pending_state.ready[s_prompt_slot - 1]) {
            player = s_prompt_slot;
        } else if (data[5] >= 1 && data[5] <= 4) {
            player = data[5];
        }
        if (player < 1 || player > 4) {
            goto queue_packet;
        }
        // New protocol: data_low = joystick ID
        const uint8_t stick_id = (data[5] >= 1 && data[5] <= 4) ? data[5] : player;
        const uint32_t color = stick_color(stick_id);
        portENTER_CRITICAL(&s_ui_mux);
        s_ready_mask |= (1u << (player - 1));
        s_ready_mask_dirty = true;
        s_prompt_mask &= ~(1u << (player - 1));
        s_prompt_mask_dirty = true;
        if (s_prompt_slot == player) {
            s_prompt_slot = 0;
        }
        s_pending_state.ready[player - 1] = true;
        if (s_pending_state.mode == ScreenMode::NONE ||
            s_pending_state.mode == ScreenMode::IDLE) {
            s_pending_state.mode = ScreenMode::PROMPT;
        }
        s_state_dirty = true;
        portEXIT_CRITICAL(&s_ui_mux);
        set_panel_border_color(player, color);
        ESP_LOGI(kTag, "FAST READY slot=%u stick=%u color=0x%06X", player, stick_id, (unsigned)color);
    }
    if (data[3] == DISP_PLAYER_PROMPT && data[5] >= 1 && data[5] <= 4) {
        const uint8_t player = data[5];
        portENTER_CRITICAL(&s_ui_mux);
        s_prompt_mask = (1u << (player - 1));
        s_prompt_mask_dirty = true;
        s_prompt_slot = player;
        if (s_pending_state.mode == ScreenMode::NONE ||
            s_pending_state.mode == ScreenMode::IDLE) {
            s_pending_state.mode = ScreenMode::PROMPT;
        }
        s_state_dirty = true;
        portEXIT_CRITICAL(&s_ui_mux);
    }
queue_packet:
    portENTER_CRITICAL(&s_ui_mux);
    memcpy(s_pending_pkt, data, PACKET_SIZE);
    s_pkt_pending = true;
    portEXIT_CRITICAL(&s_ui_mux);
}

static void init_espnow() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(kEspnowChannel, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, kHostMac, 6);
    peerInfo.channel = kEspnowChannel;
    peerInfo.encrypt = false;
    peerInfo.ifidx = WIFI_IF_STA;
    err = esp_now_add_peer(&peerInfo);
    if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGI(kTag, "Host paired");
    } else {
        ESP_LOGW(kTag, "Host pair failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(kTag, "ESP-NOW ready on channel %u", kEspnowChannel);
}

extern "C" void app_main(void) {
    // Waveshare demo base + full UI, then update one label to check flicker.
    waveshare_esp32_s3_rgb_lcd_init();

    if (lvgl_port_lock(-1)) {
        ui_init();
        show_idle();
        reset_panel_borders();
        // if (ui_labelCountDown) {
        //     lv_label_set_text(ui_labelCountDown, "0");
        //     lv_obj_clear_flag(ui_labelCountDown, LV_OBJ_FLAG_HIDDEN);
        // }
        lv_timer_t *state_timer = lv_timer_create(ui_timer_cb, 100, NULL);
        (void)state_timer;
        lvgl_port_unlock();
    } else {
        // LVGL lock failed
    }

    if (kEnableEspNow) {
        init_espnow();
    } else {
        // ESP-NOW disabled
    }
}
    
