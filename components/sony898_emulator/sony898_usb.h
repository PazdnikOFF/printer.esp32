#pragma once
#include <stdbool.h>
#include "esp_err.h"

/*
 * Initialize TinyUSB as Sony UP-D898MD_X898MD USB printer.
 * Call once at startup before any other sony898_* functions.
 */
esp_err_t sony898_usb_init(void);

/*
 * Returns true if ESP32-S3 USB Device has been seen by a USB host
 * (i.e. VBUS present and bus attach/enumeration completed).
 * Non-blocking, safe to call from main loop.
 */
bool sony898_usb_is_connected(void);

/*
 * Returns true if the USB host has sent SET_CONFIGURATION and the
 * printer interface is ready to accept Bulk OUT data.
 * Non-blocking, safe to call from main loop.
 */
bool sony898_usb_is_configured(void);

/*
 * Convenience: connected AND configured AND status ready AND parser idle.
 */
bool sony898_usb_is_ready_for_print(void);
