#pragma once
#include "esp_err.h"
#include "sony898_printer_iface.h"
#include "sony898_usb.h"

/*
 * Top-level emulator configuration — all per-unit and per-deployment
 * parameters in one place.  Model-fixed values (VID/PID, protocol, error
 * codes, timing) stay in config.h and require a recompile to change.
 *
 * Usage:
 *   sony898_config_t cfg = {0};
 *   cfg.on_job_ready = my_thermal_printer_cb;
 *   ESP_ERROR_CHECK(sony898_emulator_init(&cfg));
 *   sony898_emulator_start();
 */
typedef struct {
    /*
     * Per-unit serial number.
     *
     * Used in:  USB iSerial descriptor  ·  IEEE1284 SCSNO field
     * Format:   printable ASCII, max 16 chars (padded with '-' in IEEE1284)
     *
     * NULL or "" → load from NVS (namespace "sony898", key "serial")
     *              → fallback to CFG_USB_SERIAL if NVS key not programmed
     *
     * For serial production: leave NULL here and write each unit's serial
     * once at the factory with sony898_emulator_set_serial().
     */
    const char *serial;

    /*
     * Called when a new print image is fully received in PSRAM and the
     * printer state transitions to PRINTING.
     *
     * NULL → built-in timer simulates print duration (height / CFG_PRINT_SPEED_LPS).
     *
     * The callback must not block.  Signal completion asynchronously:
     *   sony898_printer_notify_done();   // success
     *   sony898_printer_notify_error();  // hardware fault
     */
    sony898_on_job_ready_cb_t on_job_ready;

    /* USB host connect / disconnect events.  NULL = ignore. */
    sony898_usb_event_cb_t on_connect;
    sony898_usb_event_cb_t on_disconnect;
} sony898_config_t;

/*
 * Initialize all emulator submodules from cfg.
 * Must be called after nvs_flash_init() and before sony898_emulator_start().
 *
 * Initialises (in order): image buffer, parser, status, sensors, NVS counter,
 * printer interface callback, USB device.
 */
esp_err_t sony898_emulator_init(const sony898_config_t *cfg);

/*
 * Start the TinyUSB device task.
 * Call once after sony898_emulator_init().
 */
void sony898_emulator_start(void);

/* Serial number active in this session (from NVS or cfg).  Available after init(). */
const char *sony898_emulator_get_serial(void);

/*
 * Write serial number to NVS for permanent per-unit storage.
 * Does NOT affect the running session — takes effect after next reboot + init().
 *
 * Typical flow:
 *   1. Flash firmware on blank unit.
 *   2. Send "set_serial A1234567" over UART service port.
 *   3. Reboot — unit now uses A1234567 as its identity.
 */
esp_err_t sony898_emulator_set_serial(const char *serial);
