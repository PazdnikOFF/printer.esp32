#include "sony898_status.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "sony898_status";

/* ── Error-code table (one row per state) ───────────────────────────────── */

typedef struct {
    uint16_t scmde;       /* mechanical device error  */
    uint8_t  scmce;       /* mechanical cover error   */
    uint8_t  scsye;       /* system error             */
    uint16_t scjbs;       /* job status               */
    uint8_t  port_status; /* USB GET_PORT_STATUS byte */
} state_codes_t;

/*
 * Confirmed error codes from ТЗ §9 + stated error table.
 * Fields marked UNKNOWN are reasonable assumptions not confirmed
 * from real device capture.
 */
static const state_codes_t _tbl[SONY898_STATE__COUNT] = {
    [SONY898_STATE_IDLE] = {
        .scmde = 0x0000, .scmce = 0x00, .scsye = 0x00,
        .scjbs = 0x0000,
        .port_status = PRINTER_PORT_NOT_FAULT | PRINTER_PORT_SELECT,
    },
    [SONY898_STATE_RECEIVING_JOB] = {
        .scmde = 0x0000, .scmce = 0x00, .scsye = 0x00,
        .scjbs = 0x0001, /* UNKNOWN: job-in-progress value not confirmed */
        .port_status = PRINTER_PORT_NOT_FAULT | PRINTER_PORT_SELECT,
    },
    [SONY898_STATE_PRINTING] = {
        .scmde = 0x0000, .scmce = 0x00, .scsye = 0x00,
        .scjbs = 0x0001, /* UNKNOWN */
        .port_status = PRINTER_PORT_NOT_FAULT | PRINTER_PORT_SELECT,
    },
    [SONY898_STATE_JOB_DONE] = {
        .scmde = 0x0000, .scmce = 0x00, .scsye = 0x00,
        .scjbs = 0x0000,
        .port_status = PRINTER_PORT_NOT_FAULT | PRINTER_PORT_SELECT,
    },
    [SONY898_STATE_COVER_OPEN] = {
        .scmde = CFG_ERR_COVER_OPEN_SCMDE, .scmce = CFG_ERR_COVER_OPEN_SCMCE, .scsye = 0x00,
        .scjbs = 0x0000,
        .port_status = 0x00,
    },
    [SONY898_STATE_NO_PAPER] = {
        .scmde = CFG_ERR_NO_PAPER_SCMDE, .scmce = 0x00, .scsye = 0x00,
        .scjbs = 0x0000,
        .port_status = PRINTER_PORT_SELECT | PRINTER_PORT_PAPER_EMPTY,
    },
    [SONY898_STATE_NO_RIBBON] = {
        .scmde = CFG_ERR_NO_RIBBON_SCMDE, .scmce = 0x00, .scsye = 0x00,
        .scjbs = 0x0000,
        .port_status = 0x00,
    },
    [SONY898_STATE_NO_MEDIA] = {
        .scmde = CFG_ERR_NO_MEDIA_SCMDE, .scmce = 0x00, .scsye = 0x00,
        .scjbs = 0x0000,
        .port_status = 0x00,
    },
    [SONY898_STATE_MEDIA_MISMATCH] = {
        .scmde = CFG_ERR_MEDIA_MISMATCH_SCMDE, .scmce = 0x00, .scsye = 0x00,
        .scjbs = 0x0000,
        .port_status = 0x00,
    },
    [SONY898_STATE_SYSTEM_ERROR] = {
        .scmde = CFG_ERR_SYSTEM_SCMDE, .scmce = 0x00, .scsye = CFG_ERR_SYSTEM_SCSYE,
        .scjbs = 0x0000,
        .port_status = 0x00,
    },
    [SONY898_STATE_UNKNOWN_ERROR] = {
        /* UNKNOWN: placeholder for unidentified error */
        .scmde = 0xFFFF, .scmce = 0x00, .scsye = 0xFF,
        .scjbs = 0x0000,
        .port_status = 0x00,
    },
};

/* ── State names (for logging / UART) ───────────────────────────────────── */

static const char *const _state_names[SONY898_STATE__COUNT] = {
    [SONY898_STATE_IDLE]           = "idle",
    [SONY898_STATE_RECEIVING_JOB]  = "receiving_job",
    [SONY898_STATE_PRINTING]       = "printing",
    [SONY898_STATE_JOB_DONE]       = "job_done",
    [SONY898_STATE_COVER_OPEN]     = "cover_open",
    [SONY898_STATE_NO_PAPER]       = "no_paper",
    [SONY898_STATE_NO_RIBBON]      = "no_ribbon",
    [SONY898_STATE_NO_MEDIA]       = "no_media",
    [SONY898_STATE_MEDIA_MISMATCH] = "media_mismatch",
    [SONY898_STATE_SYSTEM_ERROR]   = "system_error",
    [SONY898_STATE_UNKNOWN_ERROR]  = "unknown_error",
};

/* ── Module state ────────────────────────────────────────────────────────── */

static _Atomic(sony898_state_t) _state = SONY898_STATE_IDLE;

/* Custom field overrides (cleared on set_state). */
static struct {
    bool     active;
    uint16_t scmde;
    uint8_t  scmce;
    uint8_t  scsye;
    uint16_t scjbs;
} _custom;

/*
 * Static output buffers rebuilt on each call.  No mutex: these functions are
 * called from TinyUSB class-driver callbacks which run in ISR or task context
 * depending on the ESP-IDF TinyUSB port — blocking primitives must not be used.
 * Concurrent access (e.g. UART task vs USB task) is benign: both would produce
 * the same string for the same atomic state, and the window for a torn read is
 * negligible vs the transfer latency.
 */
static char _ieee1284_buf[800];
static char _bulk_buf[160];

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void get_effective_codes(uint16_t *scmde, uint8_t *scmce,
                                 uint8_t *scsye, uint16_t *scjbs,
                                 uint8_t *port_status) {
    sony898_state_t s = atomic_load(&_state);
    if (s >= SONY898_STATE__COUNT) s = SONY898_STATE_UNKNOWN_ERROR;
    const state_codes_t *row = &_tbl[s];

    if (_custom.active) {
        *scmde = _custom.scmde;
        *scmce = _custom.scmce;
        *scsye = _custom.scsye;
        *scjbs = _custom.scjbs;
    } else {
        *scmde = row->scmde;
        *scmce = row->scmce;
        *scsye = row->scsye;
        *scjbs = row->scjbs;
    }
    *port_status = row->port_status; /* always from table, not overridable */
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void sony898_status_init(void) {
    atomic_store(&_state, SONY898_STATE_IDLE);
    _custom.active = false;
    ESP_LOGI(TAG, "status: IDLE");
}

sony898_state_t sony898_status_get_state(void) {
    return atomic_load(&_state);
}

void sony898_status_set_state(sony898_state_t state) {
    if (state >= SONY898_STATE__COUNT) return;
    _custom.active = false;
    atomic_store(&_state, state);
    ESP_LOGI(TAG, "state → %s", _state_names[state]);
}

const char *sony898_status_state_name(sony898_state_t state) {
    if (state >= SONY898_STATE__COUNT) return "?";
    return _state_names[state];
}

esp_err_t sony898_status_set_custom_fields(uint16_t scmde, uint8_t scmce,
                                            uint8_t scsye, uint16_t scjbs) {
    _custom.scmde  = scmde;
    _custom.scmce  = scmce;
    _custom.scsye  = scsye;
    _custom.scjbs  = scjbs;
    _custom.active = true;
    ESP_LOGI(TAG, "custom fields: SCMDE=%04X SCMCE=%02X SCSYE=%02X SCJBS=%04X",
             scmde, scmce, scsye, scjbs);
    return ESP_OK;
}

const char *sony898_status_get_ieee1284_id(void) {
    uint16_t scmde; uint8_t scmce, scsye, port_status; uint16_t scjbs;
    get_effective_codes(&scmde, &scmce, &scsye, &scjbs, &port_status);

    snprintf(_ieee1284_buf, sizeof(_ieee1284_buf),
        "MFG:"   CFG_DEV_MFG   ";"
        "MDL:"   CFG_DEV_MDL   ";"
        "DES:"   CFG_DEV_DES   ";"
        "CMD:"   CFG_DEV_CMD   ";"
        "CLS:PRINTER;"
        "SCDIV:" CFG_DEV_SCDIV ";"
        "SCSYV:" CFG_DEV_SCSYV ";"
        "SCSYS:" CFG_DEV_SCSYS ";"
        "SCMDS:" CFG_DEV_SCMDS ";"
        "SCSYE:%02X;"
        "SCMDE:%04X;"
        "SCMCE:%02X;"
        "SCSYI:" CFG_DEV_SCSYI ";"
        "SCSVI:" CFG_DEV_SCSVI ";"
        "SCMDI:" CFG_DEV_SCMDI ";"
        "SCSNO:" CFG_DEV_SCSNO ";"
        "SCJBS:%04X;"
        "SCCAI:" CFG_DEV_SCCAI ";"
        "SCGSI:" CFG_DEV_SCGSI ";"
        "SCQTI:" CFG_DEV_SCQTI ";"
        "SPUQI:" CFG_DEV_SPUQI ";",
        scsye, scmde, scmce, scjbs);

    return _ieee1284_buf;
}

const char *sony898_status_get_bulk_status(void) {
    uint16_t scmde; uint8_t scmce, scsye, port_status; uint16_t scjbs;
    get_effective_codes(&scmde, &scmce, &scsye, &scjbs, &port_status);

    /*
     * UNKNOWN: exact framing for the Gutenprint sonyupdneo bulk-IN status
     * response not confirmed.  Using confirmed field names from ТЗ §9.
     * SCPRS (print remaining) is derived from SCJBS — value not confirmed.
     */
    /*
     * 4-field format confirmed to work with Gutenprint sonyupdneo.
     * SCPRS (print remaining) = 0x0001 when job active, 0x0000 otherwise.
     * SCJBS is intentionally omitted — adding it breaks status parsing.
     */
    snprintf(_bulk_buf, sizeof(_bulk_buf),
             "SCMDE=%04X\r\nSCMCE=%02X\r\nSCSYE=%02X\r\nSCPRS=%04X\r\n",
             scmde, scmce, scsye,
             (scjbs != 0) ? (uint16_t)0x0001 : (uint16_t)0x0000);

    return _bulk_buf;
}

uint8_t sony898_status_get_port_status(void) {
    uint16_t scmde; uint8_t scmce, scsye, port_status; uint16_t scjbs;
    get_effective_codes(&scmde, &scmce, &scsye, &scjbs, &port_status);
    return port_status;
}

bool sony898_status_is_ready(void) {
    sony898_state_t s = atomic_load(&_state);
    return s == SONY898_STATE_IDLE || s == SONY898_STATE_JOB_DONE;
}
