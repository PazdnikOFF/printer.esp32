#include "sony898_status.h"
#include "esp_log.h"
#include <stdatomic.h>

static const char *TAG = "sony898_status";

/*
 * IEEE1284 Device ID as confirmed by ТЗ §9.
 * Fields whose decoded meaning is not confirmed are used as-is
 * (template for host compatibility, not documented as decoded).
 */
static const char _ieee1284_id[] =
    "MFG:Sony;"
    "MDL:UP-D898MD_X898MD;"
    "DES:Sony UP-D898MD_X898MD;"
    "CMD:SPJL-DS,SPDL-DS2;"
    "CLS:PRINTER;"
    "SCDIV:0100;"
    "SCSYV:01010000;"
    "SCSYS:0000001000010000000000;"       /* UNKNOWN: field meaning not confirmed */
    "SCMDS:00000500000100000000;"         /* UNKNOWN: field meaning not confirmed */
    "SCSYE:00;"
    "SCMDE:0000;"
    "SCMCE:00;"
    "SCSYI:100005001000050000000000014500;" /* UNKNOWN: field meaning not confirmed */
    "SCSVI:000204000204;"                 /* UNKNOWN: field meaning not confirmed */
    "SCMDI:200406;"                       /* UNKNOWN: field meaning not confirmed */
    "SCSNO:0000000---------;"
    "SCJBS:0000;"
    "SCCAI:00000000000000;"               /* UNKNOWN: field meaning not confirmed */
    "SCGSI:01;"
    "SCQTI:0001;"
    "SPUQI:0000;";

/*
 * Bulk IN status response.
 * UNKNOWN: exact framing/prefix required by Gutenprint sonyupdneo backend
 * not confirmed from ТЗ.  Fields: SCMDE, SCMCE, SCSYE, SCPRS from ТЗ §9.
 * If Gutenprint does not accept a print job, capture real USB traffic
 * to determine the correct framing and update this string.
 */
static const char _status_ready[] =
    "SCMDE=0000\r\n"
    "SCMCE=00\r\n"
    "SCSYE=00\r\n"
    "SCPRS=0000\r\n";

static const char _status_cover_open[] =
    "SCMDE=0000\r\n"
    "SCMCE=01\r\n"   /* MC=01 = Cover open (confirmed by Gutenprint source per ТЗ §9) */
    "SCSYE=00\r\n"
    "SCPRS=0000\r\n";

static const char _status_busy[] =
    "SCMDE=0000\r\n"
    "SCMCE=00\r\n"
    "SCSYE=00\r\n"
    "SCPRS=0001\r\n";  /* UNKNOWN: busy SCPRS value not confirmed */

typedef enum {
    STATUS_READY,
    STATUS_COVER_OPEN,
    STATUS_BUSY,
} status_state_t;

static _Atomic(status_state_t) _state = STATUS_READY;

void sony898_status_init(void) {
    _state = STATUS_READY;
    ESP_LOGI(TAG, "status: ready");
}

bool sony898_status_is_ready(void) {
    return atomic_load(&_state) == STATUS_READY;
}

void sony898_status_set_ready(void) {
    atomic_store(&_state, STATUS_READY);
    ESP_LOGI(TAG, "status: ready");
}

void sony898_status_set_cover_open(bool enabled) {
    if (enabled) {
        atomic_store(&_state, STATUS_COVER_OPEN);
        ESP_LOGW(TAG, "status: cover open");
    } else {
        atomic_store(&_state, STATUS_READY);
        ESP_LOGI(TAG, "status: cover closed -> ready");
    }
}

void sony898_status_set_busy(bool enabled) {
    if (enabled) {
        atomic_store(&_state, STATUS_BUSY);
        ESP_LOGI(TAG, "status: busy");
    } else {
        atomic_store(&_state, STATUS_READY);
        ESP_LOGI(TAG, "status: no longer busy -> ready");
    }
}

const char *sony898_status_get_ieee1284_id(void) {
    return _ieee1284_id;
}

const char *sony898_status_get_bulk_status(void) {
    switch (atomic_load(&_state)) {
        case STATUS_COVER_OPEN: return _status_cover_open;
        case STATUS_BUSY:       return _status_busy;
        default:                return _status_ready;
    }
}

uint8_t sony898_status_get_port_status(void) {
    switch (atomic_load(&_state)) {
        case STATUS_COVER_OPEN:
            return 0x00; /* fault, offline */
        case STATUS_BUSY:
            return PRINTER_PORT_SELECT; /* online but fault asserted */
        default:
            return PRINTER_PORT_NOT_FAULT | PRINTER_PORT_SELECT; /* 0x18 = ready */
    }
}
