#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * Initialize TinyUSB with full device identity.
 * Any zero/NULL value falls back to the compile-time default in config.h.
 * Call once at startup (via sony898_emulator_init).
 */
esp_err_t sony898_usb_init(uint16_t vid, uint16_t pid,
                            const char *manufacturer,
                            const char *product,
                            const char *serial);

/* Active USB identity — valid after sony898_usb_init(). */
uint16_t    sony898_usb_get_vid(void);
uint16_t    sony898_usb_get_pid(void);
const char *sony898_usb_get_manufacturer(void);
const char *sony898_usb_get_product(void);

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

/*
 * Start the TinyUSB device task.  Call once after sony898_usb_init().
 * The task runs tud_task() in a loop at highest priority.
 */
void sony898_usb_start_task(void);

/* ── USB connection event callbacks ─────────────────────────────────────────
 *
 * Register handlers to be notified of USB connect / disconnect events.
 * Both callbacks are invoked from the TinyUSB device task context —
 * keep them short and non-blocking.
 *
 * Example:
 *   sony898_usb_set_connect_cb(on_usb_connect);
 *   sony898_usb_set_disconnect_cb(on_usb_disconnect);
 */
typedef void (*sony898_usb_event_cb_t)(void);

void sony898_usb_set_connect_cb(sony898_usb_event_cb_t cb);
void sony898_usb_set_disconnect_cb(sony898_usb_event_cb_t cb);
