#pragma once
#ifdef __cplusplus

extern "C" {
#include "esp_err.h"
#include "sony898_emulator.h"
}

/*
 * Sony UP-D898MD_X898MD printer emulator — C++ interface.
 *
 * Singleton class wrapping the full emulator C API.
 * Use the global `Printer` instance.
 *
 * Usage:
 *   Printer.onJobReady = myPrintCallback;
 *   Printer.init();
 *   Printer.begin();
 *
 *   // from GPIO handler or sensor task:
 *   Printer.setPaper(gpio_get_level(PIN_PAPER));
 *
 *   // from print module task after printing:
 *   Printer.notifyDone();
 */
class Sony898Printer {
public:

    /*
     * Constructor — pass serial number to bind this instance to a specific unit.
     * nullptr / omitted → serial loaded from NVS on init(), fallback to config.h default.
     *
     * Examples:
     *   Sony898Printer Printer;               // NVS / default serial
     *   Sony898Printer Printer("A1234567");   // fixed serial for this unit
     */
    explicit Sony898Printer(const char *sn = nullptr) : serial(sn) {}

    /* ── Configuration (set before init()) ─────────────────────────────── */

    /*
     * USB device identity — all optional.
     * 0 / nullptr → compile-time default from config.h (CFG_USB_*).
     * Override for OEM or multi-model deployments.
     */
    uint16_t    vid          = 0;        /* USB Vendor  ID */
    uint16_t    pid          = 0;        /* USB Product ID */
    const char *manufacturer = nullptr;  /* iManufacturer string */
    const char *product      = nullptr;  /* iProduct     string */

    /*
     * Per-unit serial number.
     * Set via constructor or directly. nullptr → NVS → config.h fallback.
     */
    const char *serial;

    /* Print module callback.  nullptr = built-in timer mode. */
    sony898_on_job_ready_cb_t onJobReady = nullptr;

    /* USB host event handlers.  nullptr = ignore. */
    sony898_usb_event_cb_t onConnect    = nullptr;
    sony898_usb_event_cb_t onDisconnect = nullptr;

    /* ── Lifecycle ──────────────────────────────────────────────────────── */

    /* Initialize all submodules.  Call after nvs_flash_init(). */
    esp_err_t init();

    /* Start USB device task and internal status/dispatch task. */
    void begin();

    /* ── Identity ───────────────────────────────────────────────────────── */

    /* Active USB device identity — valid after init(). */
    uint16_t    getVid()          const;
    uint16_t    getPid()          const;
    const char *getManufacturer() const;
    const char *getProduct()      const;

    /* Fixed protocol and USB class strings (model-defined, not per-unit). */
    const char *getProtocol()     const;
    const char *getUsbClass()     const;

    /* Active serial number (from NVS or field serial).  Valid after init(). */
    const char *getSerial() const;

    /*
     * Write serial to NVS permanently.  Reboot required to apply.
     * Factory workflow: flash → setSerial("A1234567") → reboot.
     */
    esp_err_t setSerial(const char *s);

    /* Current IEEE1284 Device ID string. */
    const char *getDeviceId() const;

    /* ── USB state ──────────────────────────────────────────────────────── */

    bool usbIsConnected()  const;
    bool usbIsConfigured() const;

    /* True when connected + configured + printer ready + parser idle. */
    bool usbIsReady() const;

    /* ── Printer state ──────────────────────────────────────────────────── */

    sony898_state_t getState()      const;
    void            setState(sony898_state_t state);
    const char     *getStateName()  const;
    uint8_t         getPortStatus() const;
    bool            isPrinterReady() const;

    /* ── Image / job ────────────────────────────────────────────────────── */

    bool           imageReady()    const;
    uint16_t       getWidth()      const;
    uint16_t       getHeight()     const;
    size_t         getImageSize()  const;

    /* Pointer to grayscale image in PSRAM.  Valid until clearJob() or next job. */
    const uint8_t *getImageBuffer() const;

    /* Copy count from PDL header (always >= 1). */
    uint8_t getCopies() const;

    /* Release image buffer and reset parser to accept a new job. */
    void clearJob();

    /* ── Sensor inputs ──────────────────────────────────────────────────── */
    /*
     * Call from GPIO interrupt handlers or a sensor monitoring task.
     * Priority: systemError > coverOpen > noRibbon > noPaper > mediaMatch
     */

    void setPaper(bool present);       /* false → NO_PAPER       */
    void setCover(bool closed);        /* false → COVER_OPEN     */
    void setRibbon(bool present);      /* false → NO_RIBBON      */
    void setMediaMatch(bool matches);  /* false → MEDIA_MISMATCH */
    void setSystemError(bool error);   /* true  → SYSTEM_ERROR   */
    void clearSensors();               /* clear all → back to IDLE */
    bool hasSensorError() const;

    /* ── Print completion ───────────────────────────────────────────────── */

    /* Call from the print module task when printing finishes successfully. */
    void notifyDone();

    /* Call from the print module task on hardware fault. */
    void notifyError();

    /* True if a print module callback is registered (vs. built-in timer). */
    bool hasPrintModule() const;

    /* ── Statistics ─────────────────────────────────────────────────────── */

    /* Total completed print jobs (NVS-persistent, survives reboot). */
    uint32_t getPrintCount() const;
};

/* Global singleton — use this instance everywhere. */
extern Sony898Printer Printer;

#endif /* __cplusplus */
