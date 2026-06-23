#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * USB Printer Class port-status bits (GET_PORT_STATUS request).
 * Bit 3 – NOT FAULT (1 = no fault)
 * Bit 4 – SELECT    (1 = online)
 * Bit 5 – PAPER EMPTY (1 = no paper; we always report paper present)
 */
#define PRINTER_PORT_NOT_FAULT 0x08u
#define PRINTER_PORT_SELECT    0x10u
#define PRINTER_PORT_PAPER_EMPTY 0x20u

void    sony898_status_init(void);
bool    sony898_status_is_ready(void);
void    sony898_status_set_ready(void);
void    sony898_status_set_cover_open(bool enabled);
void    sony898_status_set_busy(bool enabled);

/* IEEE1284 Device ID string (prepend 2-byte BE length before sending). */
const char *sony898_status_get_ieee1284_id(void);

/*
 * Status response text sent via Bulk IN when the host queries printer status.
 * UNKNOWN: exact framing required by Gutenprint sonyupdneo backend
 * not confirmed — using field values from ТЗ §9.
 */
const char *sony898_status_get_bulk_status(void);

/* 1-byte port status for USB Printer Class GET_PORT_STATUS. */
uint8_t sony898_status_get_port_status(void);
