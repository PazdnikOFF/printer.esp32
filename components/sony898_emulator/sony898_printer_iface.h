#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Interface between the USB receive module and the external print module.
 *
 * ┌─────────────────────────────────────────────────────────────┐
 * │                       Flow                                  │
 * │                                                             │
 * │  USB host → [sony898_emulator] → on_job_ready callback      │
 * │                                       │                     │
 * │                              [print module]                 │
 * │                                       │                     │
 * │  sony898_printer_notify_done() ←──────┘                     │
 * └─────────────────────────────────────────────────────────────┘
 *
 * The PSRAM buffer (job->buf) stays valid until the next call to
 * sony898_parser_reset() or until a new job arrives.
 * The print module must NOT free it.
 */

typedef struct {
    const uint8_t *buf;     /* PSRAM pointer — read-only for print module */
    uint16_t       width;   /* image width in pixels                       */
    uint16_t       height;  /* image height in pixels                      */
    size_t         size;    /* width * height bytes (grayscale 8-bit)      */
    uint8_t        copies;  /* copy count from PDL header; always >= 1     */
} sony898_print_job_t;

/*
 * Callback type invoked when a new image is ready for printing.
 * Called from the status_log_task context — return quickly.
 * Do NOT call sony898_printer_notify_done() from inside the callback.
 */
typedef void (*sony898_on_job_ready_cb_t)(const sony898_print_job_t *job);

/* ── Print module API ────────────────────────────────────────────────────── */

/* Register the print module's job-ready callback (call once at system init). */
void sony898_printer_iface_set_callback(sony898_on_job_ready_cb_t cb);

/*
 * Call when physical printing is complete.
 * Transitions state: PRINTING → JOB_DONE.
 * Safe to call from any task context (uses atomics).
 */
void sony898_printer_notify_done(void);

/*
 * Call when a hardware print error occurred (jam, overtemp, etc.).
 * Transitions state to SYSTEM_ERROR.
 * The host will see the error on the next GET_DEVICE_ID poll.
 * Safe to call from any task context.
 */
void sony898_printer_notify_error(void);

/* ── Internal API (used by status_log_task in main.c) ───────────────────── */

/* True if a print module callback has been registered. */
bool sony898_printer_iface_has_module(void);

/*
 * Dispatch a job to the registered callback.
 * Resets done/error flags.
 * Returns false if no callback is registered.
 */
bool sony898_printer_iface_dispatch(const sony898_print_job_t *job);

/*
 * Check and consume the done flag (atomic test-and-clear).
 * Returns true once after notify_done() was called.
 */
bool sony898_printer_iface_check_done(void);

/*
 * Check and consume the error flag (atomic test-and-clear).
 * Returns true once after notify_error() was called.
 */
bool sony898_printer_iface_check_error(void);
