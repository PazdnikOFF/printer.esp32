#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* ── USB Printer Class port-status byte (GET_PORT_STATUS) ───────────────── */
#define PRINTER_PORT_NOT_FAULT   0x08u   /* 1 = no fault        */
#define PRINTER_PORT_SELECT      0x10u   /* 1 = printer online  */
#define PRINTER_PORT_PAPER_EMPTY 0x20u   /* 1 = no paper        */

/* ── Printer state ───────────────────────────────────────────────────────── */
typedef enum {
    SONY898_STATE_IDLE,
    SONY898_STATE_RECEIVING_JOB,
    SONY898_STATE_PRINTING,
    SONY898_STATE_JOB_DONE,
    SONY898_STATE_COVER_OPEN,
    SONY898_STATE_NO_PAPER,
    SONY898_STATE_NO_RIBBON,
    SONY898_STATE_NO_MEDIA,
    SONY898_STATE_MEDIA_MISMATCH,
    SONY898_STATE_SYSTEM_ERROR,
    SONY898_STATE_UNKNOWN_ERROR,
    SONY898_STATE__COUNT,
} sony898_state_t;

/* ── Module init ─────────────────────────────────────────────────────────── */
void sony898_status_init(void);

/* ── State get/set ───────────────────────────────────────────────────────── */
sony898_state_t sony898_status_get_state(void);
void            sony898_status_set_state(sony898_state_t state);
const char     *sony898_status_state_name(sony898_state_t state);

/*
 * Build IEEE1284 Device ID string for the given state into out[out_len].
 * The string uses field separators "FIELD:value;" as per IEEE1284.
 * Dynamic fields (SCSYE, SCMDE, SCMCE, SCJBS) are filled from the
 * state table.  Unconfirmed fields are kept as compatibility templates.
 * Returns ESP_ERR_INVALID_SIZE if out_len is too small.
 */
esp_err_t sony898_status_build_ieee1284(
    sony898_state_t state,
    char *out,
    size_t out_len
);

/*
 * Override error-code fields for the current state.
 * Overrides are cleared when sony898_status_set_state() is called.
 * Use when exact codes are known from real USB traffic capture.
 * UNKNOWN: only SCMDE, SCMCE, SCSYE, SCJBS confirmed from ТЗ.
 */
esp_err_t sony898_status_set_custom_fields(
    uint16_t scmde,
    uint8_t  scmce,
    uint8_t  scsye,
    uint16_t scjbs
);

/* ── Accessors used by the USB layer ─────────────────────────────────────── */

/* IEEE1284 Device ID string for current state (no length prefix). */
const char *sony898_status_get_ieee1284_id(void);

/*
 * Bulk IN status response for current state.
 * UNKNOWN: exact framing expected by Gutenprint not confirmed.
 * Fields: SCMDE, SCMCE, SCSYE, SCJBS, SCPRS from ТЗ §9.
 */
const char *sony898_status_get_bulk_status(void);

/* 1-byte USB Printer Class port status for current state. */
uint8_t sony898_status_get_port_status(void);

/* True when printer can accept a new job (IDLE or JOB_DONE). */
bool sony898_status_is_ready(void);
