#pragma once
#include "esp_err.h"
#include "sony898_status.h"
#include "sony898_usb.h"
#include "sony898_printer_iface.h"

/*
 * Sony UP-D898MD_X898MD USB printer emulator — complete public API.
 *
 * Include only this header in your project.  All functions are prefixed
 * sony898_emulator_* so they can be used alongside any other component
 * without namespace conflicts.
 *
 * Minimal integration:
 *
 *   sony898_config_t cfg = {0};
 *   cfg.on_job_ready = my_thermal_print_callback;
 *   ESP_ERROR_CHECK(sony898_emulator_init(&cfg));
 *   sony898_emulator_start();
 *
 * From the thermal print module task, after printing:
 *   sony898_emulator_notify_done();   // or notify_error() on fault
 *
 * From GPIO sensor handlers:
 *   sony898_emulator_set_paper(gpio_get_level(PIN_PAPER));
 *   sony898_emulator_set_cover(gpio_get_level(PIN_COVER));
 */

/* ── Configuration ───────────────────────────────────────────────────────── */

typedef struct {
    /*
     * USB device identity — all optional.
     * NULL/0 → use compile-time default from config.h (CFG_USB_*).
     * Set for OEM or multi-model deployments.
     */
    uint16_t    vid;          /* USB Vendor  ID. 0 → CFG_USB_VID  */
    uint16_t    pid;          /* USB Product ID. 0 → CFG_USB_PID  */
    const char *manufacturer; /* iManufacturer string. NULL → CFG_USB_MANUFACTURER */
    const char *product;      /* iProduct     string. NULL → CFG_USB_PRODUCT */

    /*
     * Per-unit serial number (USB iSerial + IEEE1284 SCSNO).
     * NULL → read from NVS ("sony898"/"serial"), fallback to CFG_USB_SERIAL.
     * Write permanently with sony898_emulator_set_serial() at the factory.
     */
    const char *serial;

    /*
     * Called when a new image is fully received in PSRAM and the emulator
     * transitions to PRINTING state.  Must not block.
     * NULL → built-in timer (height / CFG_PRINT_SPEED_LPS seconds).
     */
    sony898_on_job_ready_cb_t on_job_ready;

    /* USB host connect / disconnect events.  NULL = ignore. */
    sony898_usb_event_cb_t on_connect;
    sony898_usb_event_cb_t on_disconnect;
} sony898_config_t;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/* Initialize all submodules.  Call after nvs_flash_init(). */
esp_err_t sony898_emulator_init(const sony898_config_t *cfg);

/* Start USB device task and internal status/dispatch task. */
void sony898_emulator_start(void);

/* ── Identity ────────────────────────────────────────────────────────────── */

/* Active USB device identity — valid after init(). */
uint16_t    sony898_emulator_get_vid(void);
uint16_t    sony898_emulator_get_pid(void);
const char *sony898_emulator_get_manufacturer(void);
const char *sony898_emulator_get_product(void);

/* Protocol and USB class strings (model-fixed, not per-unit). */
const char *sony898_emulator_get_protocol(void);
const char *sony898_emulator_get_usb_class(void);

/* Active serial number (from cfg or NVS).  Available after init(). */
const char *sony898_emulator_get_serial(void);

/*
 * Write serial to NVS permanently.  Reboot required to apply.
 * Factory use: flash firmware → sony898_emulator_set_serial("A1234567") → reboot.
 */
esp_err_t sony898_emulator_set_serial(const char *serial);

/* Current IEEE1284 Device ID string (reflects active printer state). */
const char *sony898_emulator_get_ieee1284_id(void);

/* ── USB state ───────────────────────────────────────────────────────────── */

bool sony898_emulator_usb_is_connected(void);
bool sony898_emulator_usb_is_configured(void);

/* True when connected + configured + printer ready + parser idle. */
bool sony898_emulator_usb_is_ready(void);

/* ── Printer state ───────────────────────────────────────────────────────── */

sony898_state_t sony898_emulator_get_state(void);

/* Force state — for testing or from sensor handlers via the sensor API. */
void sony898_emulator_set_state(sony898_state_t state);

/* Human-readable state name ("idle", "printing", "cover_open", …). */
const char *sony898_emulator_get_state_name(void);

/* 1-byte USB Printer Class GET_PORT_STATUS byte for current state. */
uint8_t sony898_emulator_get_port_status(void);

/* True when printer can accept a new job (IDLE or JOB_DONE). */
bool sony898_emulator_is_printer_ready(void);

/* ── Image / job ─────────────────────────────────────────────────────────── */

bool           sony898_emulator_image_ready(void);
uint16_t       sony898_emulator_get_width(void);
uint16_t       sony898_emulator_get_height(void);
size_t         sony898_emulator_get_image_size(void);

/* Pointer to grayscale image in PSRAM.  Valid until clear_job() or next job. */
const uint8_t *sony898_emulator_get_image_buffer(void);

/* Copy count from PDL header (always >= 1). */
uint8_t sony898_emulator_get_copies(void);

/* Release image buffer and reset parser to accept a new job. */
void sony898_emulator_clear_job(void);

/* ── Sensor inputs ───────────────────────────────────────────────────────── */
/*
 * Call from GPIO interrupt handlers or a sensor monitoring task.
 * Each call immediately updates printer state visible to the USB host.
 *
 * Error priority: system_error > cover_open > no_ribbon > no_paper > media_mismatch
 */

void sony898_emulator_set_paper(bool present);       /* false → NO_PAPER       */
void sony898_emulator_set_cover(bool closed);        /* false → COVER_OPEN     */
void sony898_emulator_set_ribbon(bool present);      /* false → NO_RIBBON      */
void sony898_emulator_set_media_match(bool matches); /* false → MEDIA_MISMATCH */
void sony898_emulator_set_system_error(bool error);  /* true  → SYSTEM_ERROR   */
void sony898_emulator_clear_sensors(void);           /* clear all errors → IDLE */
bool sony898_emulator_has_sensor_error(void);

/* ── Print completion ────────────────────────────────────────────────────── */

/* Call from the print module task when printing finishes successfully. */
void sony898_emulator_notify_done(void);

/* Call from the print module task on hardware fault. */
void sony898_emulator_notify_error(void);

/* True if a print module callback is registered (vs. built-in timer). */
bool sony898_emulator_has_print_module(void);

/* ── Statistics ──────────────────────────────────────────────────────────── */

/* Total successfully completed print jobs (NVS-persistent). */
uint32_t sony898_emulator_get_print_count(void);
