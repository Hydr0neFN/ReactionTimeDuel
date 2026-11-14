#include "net.h"

#include <atomic>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs_flash.h"

#include "diag.h"

#include "RtdNetConfig.h"
#include "RtdProtocol.h"
#include "RtdService.h"

static const char *TAG = "net";

static const uint8_t kDefaultHostMac[6] = RTD_MAC_HOST;
static const uint32_t kStaRetryMs = 2000;

static uint8_t s_host_mac[6];
static QueueHandle_t s_queue;
static esp_netif_t *s_sta_netif;
/* s_sta_ip / s_ip_last are written by the Wi-Fi event task, everything else by the LVGL task. */
static std::atomic<bool> s_sta_wanted{false};
static std::atomic<bool> s_sta_ip{false};
static std::atomic<uint8_t> s_ip_last{0};
static uint32_t s_last_try_ms;

static uint32_t now_ms()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- ports of DisplayLink ---- */

class NetPorts : public DisplayPorts {
public:
    uint32_t millis() override { return now_ms(); }
    void send(const uint8_t *data, uint8_t len) override
    {
        esp_now_send(s_host_mac, data, len);
    }
};

static NetPorts s_ports;

DisplayPorts &net_ports() { return s_ports; }

/* ---- ESP-NOW ---- */

/* No logging and no blocking call here (DESIGN.md 3.3). */
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!info || !data || !s_queue) return;
    if (len != PACKET_SIZE && len != STATE_SIZE && len != SVC_SIZE) return;
    if (data[0] != (len == STATE_SIZE ? STATE_START : len == SVC_SIZE ? SVC_START : PACKET_START)) return;

    NetFrame f;
    memcpy(f.mac, info->src_addr, 6);
    f.len = (uint8_t)len;
    memcpy(f.data, data, (size_t)len);
    f.rx_ms = now_ms();
    if (xQueueSend(s_queue, &f, 0) != pdTRUE) diag_rx_drop();
}

/* Counts failed sends only; no logging here either. */
static void on_data_sent(const uint8_t *, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS) diag_tx_fail();
}

static void add_host_peer(const uint8_t *mac)
{
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = 0; /* current channel: 6, also while the station is connected */
    peer.encrypt = false;
    peer.ifidx = WIFI_IF_STA;
    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGW(TAG, "host peer failed: %s", esp_err_to_name(err));
    }
}

bool net_pop(NetFrame &f)
{
    return s_queue && xQueueReceive(s_queue, &f, 0) == pdTRUE;
}

void net_learn_host(const NetFrame &f)
{
    if (memcmp(f.mac, s_host_mac, 6) == 0) return;
    bool from_host;
    if (f.len == STATE_SIZE) {
        from_host = validateState(f.data, f.len) != 0;
    } else if (f.len == SVC_SIZE) {
        from_host = validateService(f.data, f.len) != 0;
    } else {
        from_host = validatePacket(f.data, f.len) && f.data[2] == ID_HOST &&
                    (f.data[1] == ID_DISPLAY || f.data[1] == ID_BROADCAST);
    }
    if (!from_host) return;
    esp_now_del_peer(s_host_mac);
    memcpy(s_host_mac, f.mac, 6);
    add_host_peer(s_host_mac);
    ESP_LOGI(TAG, "host address learned %02X:%02X:%02X:%02X:%02X:%02X", f.mac[0], f.mac[1], f.mac[2],
             f.mac[3], f.mac[4], f.mac[5]);
}

/* ---- station (maintenance mode) ---- */

static void on_wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_ip = false;
        s_ip_last = 0;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        s_ip_last = (uint8_t)((ev->ip_info.ip.addr >> 24) & 0xFF); /* addr is in network byte order */
        s_sta_ip = true;
    }
}

void net_sta_enable(bool on)
{
    if (on == s_sta_wanted) return;
    s_sta_wanted = on;
    s_last_try_ms = now_ms() - kStaRetryMs; /* try at once */
    if (!on) {
        esp_wifi_disconnect();
        s_sta_ip = false;
        s_ip_last = 0;
    }
}

void net_tick()
{
    if (!s_sta_wanted || s_sta_ip) return;
    uint32_t now = now_ms();
    if ((uint32_t)(now - s_last_try_ms) < kStaRetryMs) return;
    s_last_try_ms = now;
    wifi_config_t cfg = {};
    strncpy((char *)cfg.sta.ssid, RTD_AP_SSID, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, RTD_AP_PASS, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.channel = ESPNOW_CHANNEL;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect(); /* fails harmlessly while a previous attempt is still running */
}

bool net_sta_connected() { return s_sta_ip; }

uint8_t net_ip_last_octet() { return s_sta_ip ? s_ip_last.load() : 0; }

/* ---- init ---- */

void net_init()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    memcpy(s_host_mac, kDefaultHostMac, 6);
    s_queue = xQueueCreate(NET_QUEUE_LEN, sizeof(NetFrame));
    configASSERT(s_queue);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    (void)s_sta_netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    add_host_peer(s_host_mac);
    ESP_LOGI(TAG, "ESP-NOW ready on channel %d", ESPNOW_CHANNEL);
}
