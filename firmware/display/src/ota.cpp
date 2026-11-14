#include "ota.h"

#include <atomic>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "RtdProtocol.h"

static const char *TAG = "ota";

/* The version part is not checked: any 4.x display image is a display image. */
static const char kMarker[] = "RTDFW|display|";
static const size_t kMarkerLen = sizeof(kMarker) - 1;

/* Consecutive httpd_req_recv timeouts (recv_wait_timeout each) tolerated before the upload is dropped. */
static const int kMaxRecvTimeouts = 3;

static httpd_handle_t s_server; /* touched only by the LVGL task (start / stop) */

/*
 * Shared between the httpd task (update_handler) and the LVGL task (UI timer):
 * everything below is std::atomic. The httpd task never calls LVGL or DisplayLink.
 */
static std::atomic<uint8_t> s_state{ODS_OFF};
static std::atomic<uint8_t> s_progress{0};
static std::atomic<bool> s_in_handler{false}; /* true for the whole life of update_handler */

OtaDisplayState ota_state() { return (OtaDisplayState)s_state.load(); }
uint8_t ota_progress() { return s_progress.load(); }
bool ota_busy() { return s_in_handler.load() || s_state.load() == ODS_RECEIVING; }

static void set_cors(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *body)
{
    set_cors(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *body)
{
    s_state = ODS_FAILED;
    ESP_LOGW(TAG, "upload rejected: %s", body);
    reply(req, status, body);
    return ESP_FAIL; /* httpd closes the socket, so unread body bytes cannot confuse the next request */
}

static esp_err_t options_handler(httpd_req_t *req)
{
    set_cors(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t index_handler(httpd_req_t *req)
{
    return reply(req, "200 OK", "RTD display OTA: POST the raw display.bin to /update\n");
}

struct HandlerGuard {
    HandlerGuard() { s_in_handler = true; }
    ~HandlerGuard() { s_in_handler = false; }
};

static esp_err_t update_handler(httpd_req_t *req)
{
    HandlerGuard guard;

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return fail(req, "500 Internal Server Error", "no update partition");
    }
    size_t total = req->content_len;
    if (total == 0 || total > part->size) {
        return fail(req, "400 Bad Request", "bad image size");
    }

    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(part, total, &handle) != ESP_OK) { /* no handle is left behind on failure */
        return fail(req, "500 Internal Server Error", "ota begin failed");
    }
    s_state = ODS_RECEIVING;
    s_progress = 0;

    /* Overlap buffer: the marker may straddle two chunks. */
    static uint8_t buf[2048 + 16];
    size_t keep = 0, received = 0;
    bool marker = false, io_error = false, timed_out = false;
    int timeouts = 0;

    while (received < total) {
        size_t want = total - received;
        if (want > 2048) want = 2048;
        int n = httpd_req_recv(req, (char *)buf + keep, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts >= kMaxRecvTimeouts) {
                timed_out = true;
                break;
            }
            continue;
        }
        timeouts = 0;
        if (n <= 0) {
            io_error = true;
            break;
        }
        if (esp_ota_write(handle, buf + keep, (size_t)n) != ESP_OK) {
            io_error = true;
            break;
        }
        size_t have = keep + (size_t)n;
        if (!marker && have >= kMarkerLen) {
            for (size_t i = 0; i + kMarkerLen <= have; i++) {
                if (memcmp(buf + i, kMarker, kMarkerLen) == 0) {
                    marker = true;
                    break;
                }
            }
        }
        keep = have >= kMarkerLen - 1 ? kMarkerLen - 1 : have;
        memmove(buf, buf + have - keep, keep);
        received += (size_t)n;
        s_progress = (uint8_t)((uint64_t)received * 100 / total);
    }

    /* From here on the handle is live: every failure below aborts it exactly once. */
    if (timed_out) {
        esp_ota_abort(handle);
        return fail(req, "408 Request Timeout", "upload stalled");
    }
    if (io_error) {
        esp_ota_abort(handle);
        return fail(req, "500 Internal Server Error", "upload interrupted");
    }
    if (!marker) {
        esp_ota_abort(handle);
        return fail(req, "400 Bad Request", "not a display image");
    }
    /* esp_ota_end releases the handle on every outcome, so a failure here needs no abort. */
    if (esp_ota_end(handle) != ESP_OK) {
        return fail(req, "400 Bad Request", "image check failed");
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        return fail(req, "500 Internal Server Error", "set boot partition failed");
    }

    s_progress = 100;
    s_state = ODS_DONE;
    ESP_LOGI(TAG, "image accepted (%u bytes), rebooting", (unsigned)total);
    esp_err_t err = reply(req, "200 OK", "OK");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return err;
}

void ota_server_start()
{
    if (s_server) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.recv_wait_timeout = 10;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        s_server = NULL;
        ESP_LOGW(TAG, "http server start failed");
        return;
    }
    static const httpd_uri_t post = { "/update", HTTP_POST, update_handler, NULL };
    static const httpd_uri_t opt = { "/update", HTTP_OPTIONS, options_handler, NULL };
    static const httpd_uri_t idx = { "/", HTTP_GET, index_handler, NULL };
    httpd_register_uri_handler(s_server, &post);
    httpd_register_uri_handler(s_server, &opt);
    httpd_register_uri_handler(s_server, &idx);
    s_state = ODS_READY;
}

bool ota_server_stop()
{
    if (!s_server) return true;
    if (ota_busy()) return false; /* httpd_stop would block until the handler ends; the caller retries later */
    httpd_stop(s_server);
    s_server = NULL;
    s_state = ODS_OFF;
    return true;
}

void ota_mark_valid()
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "app marked valid");
    }
}
