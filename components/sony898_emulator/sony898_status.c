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
 * Pre-built response buffers — rebuilt only on state change, not on every query.
 * GET_DEVICE_ID and bulk IN are polled frequently by the host; snprintf on each
 * call adds unnecessary load to the USB callback path.
 *
 * Thread safety: rebuilt in set_state() which may race with USB callbacks reading
 * the buffers.  The worst case is one stale response during a state transition —
 * acceptable since the host re-polls within milliseconds.
 */
static char   _ieee1284_buf[800];
static size_t _ieee1284_len;
static char   _bulk_buf[160];
static size_t _bulk_len;

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
    *port_status = row->port_status;
}

static void rebuild_cache(void) {
    uint16_t scmde; uint8_t scmce, scsye, port_status; uint16_t scjbs;
    get_effective_codes(&scmde, &scmce, &scsye, &scjbs, &port_status);

    _ieee1284_len = (size_t)snprintf(_ieee1284_buf, sizeof(_ieee1284_buf),
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

    _bulk_len = (size_t)snprintf(_bulk_buf, sizeof(_bulk_buf),
        "SCMDE=%04X\r\nSCMCE=%02X\r\nSCSYE=%02X\r\nSCPRS=%04X\r\n",
        scmde, scmce, scsye,
        (scjbs != 0) ? (uint16_t)0x0001 : (uint16_t)0x0000);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void sony898_status_init(void) {
    atomic_store(&_state, SONY898_STATE_IDLE);
    _custom.active = false;
    rebuild_cache();
    ESP_LOGI(TAG, "status: IDLE");
}

sony898_state_t sony898_status_get_state(void) {
    return atomic_load(&_state);
}

void sony898_status_set_state(sony898_state_t state) {
    if (state >= SONY898_STATE__COUNT) return;
    _custom.active = false;
    atomic_store(&_state, state);
    rebuild_cache();
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
    rebuild_cache();
    ESP_LOGI(TAG, "custom fields: SCMDE=%04X SCMCE=%02X SCSYE=%02X SCJBS=%04X",
             scmde, scmce, scsye, scjbs);
    return ESP_OK;
}

const char *sony898_status_get_ieee1284_id(void) {
    return _ieee1284_buf;
}

size_t sony898_status_get_ieee1284_len(void) {
    return _ieee1284_len;
}

const char *sony898_status_get_bulk_status(void) {
    return _bulk_buf;
}

size_t sony898_status_get_bulk_len(void) {
    return _bulk_len;
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
