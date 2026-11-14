#pragma once

/*
 * Radio side of the display: Wi-Fi station + ESP-NOW on channel 6 (DESIGN.md 3, 7.5, 9.5).
 * The ESP-NOW receive callback only copies the frame into a FreeRTOS queue; the LVGL timer drains it.
 */

#include <stdint.h>
#include "DisplayLink.h"
#include "RtdService.h"

#define NET_QUEUE_LEN 16

/* len is PACKET_SIZE, STATE_SIZE or SVC_SIZE (the service report, DESIGN.md 12). */
struct NetFrame {
    uint8_t mac[6];
    uint8_t len;
    uint8_t data[SVC_SIZE];
    uint32_t rx_ms;
};

/* NVS, netif, Wi-Fi (STA, channel 6, power save off), ESP-NOW, receive queue. Call once. */
void net_init();

/* DisplayPorts of the link: millis() and send() to the host. */
DisplayPorts &net_ports();

/* Pop one received frame (non-blocking). Returns false when the queue is empty. */
bool net_pop(NetFrame &f);

/* Address learning (DESIGN.md 3.2): when this frame is a valid host frame from another MAC, replace the host peer. */
void net_learn_host(const NetFrame &f);

/* Maintenance mode: join / leave the host access point as a station; ESP-NOW keeps running.
 * Disabling calls esp_wifi_disconnect(): do not call it from the LVGL task while ota_busy() (ota.h). */
void net_sta_enable(bool on);

/* Retry the station connection while it is wanted; call from the UI timer. */
void net_tick();

/* True when the station has an IP address; net_ip_last_octet() is its last byte (0 when not connected). */
bool net_sta_connected();
uint8_t net_ip_last_octet();
