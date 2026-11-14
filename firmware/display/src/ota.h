#pragma once

/*
 * Display OTA (DESIGN.md 9.5): HTTP server with POST /update that streams the image into the
 * inactive OTA slot. Runs only in maintenance mode (PH_MAINT).
 */

#include <stdint.h>

/* Value of the "display ota state" byte of CMD_DISPLAY_INFO (chosen by the display, the host only shows it). */
enum OtaDisplayState : uint8_t {
    ODS_OFF = 0,        /* normal mode, or maintenance mode without an address yet */
    ODS_READY = 1,      /* joined the AP, waiting for an upload */
    ODS_RECEIVING = 2,  /* image upload running */
    ODS_DONE = 3,       /* image accepted, rebooting */
    ODS_FAILED = 4,     /* last upload rejected */
};

/* Start / stop the HTTP server. Safe to call repeatedly. ota_server_stop() blocks (httpd_stop joins the
 * handler thread), so it is refused (returns false) while ota_busy(); the caller retries later. */
void ota_server_start();
bool ota_server_stop(); /* true when the server is stopped (or was not running), false when deferred */

/* Current state and upload progress 0..100 (thread safe, atomic reads). */
OtaDisplayState ota_state();
uint8_t ota_progress();

/* True while an upload handler is running (receiving, or accepted and about to reboot). */
bool ota_busy();

/* Mark the running image valid (cancels the rollback). Call 10 s after the UI is up. */
void ota_mark_valid();
