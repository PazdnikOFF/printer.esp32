#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/*
 * Parser state machine for the SPDL-DS2 USB stream.
 *
 * Gutenprint's gutenprint53+usb backend strips the CUPS spool JOBSIZE= headers
 * and sends only the raw block contents over USB in this order:
 *   1. PJL header  (ASCII text, ends with "@PJL ENTER LANGUAGE=SONY-PDL-DS2\r\n")
 *   2. PDL block   (binary: 290-byte header + pixels + 7-byte footer)
 *   3. PJL trailer (ASCII text with "@PJL EOJ")
 */
typedef enum {
    PARSER_SCAN_PJL_HEADER = 0, /* scanning for PJL ENTER LANGUAGE marker */
    PARSER_READ_PDL_HEADER,     /* accumulating 290-byte binary PDL header  */
    PARSER_READ_IMAGE_PAYLOAD,  /* streaming pixel data to PSRAM            */
    PARSER_READ_PDL_FOOTER,     /* discarding 7-byte PDL footer             */
    PARSER_JOB_COMPLETE,        /* image ready; waiting for auto-idle timer */
    PARSER_ERROR,
} parser_state_t;

void           sony898_parser_init(void);
void           sony898_parser_reset(void);
esp_err_t      sony898_parser_feed(const uint8_t *data, size_t len);
bool           sony898_parser_can_accept_job(void);
parser_state_t sony898_parser_get_state(void);
uint8_t        sony898_parser_get_copies(void); /* from PDL header; always >= 1 */

/*
 * Reset parser to accept a new job without releasing the image buffer.
 * Called automatically after CFG_JOB_DONE_IDLE_DELAY_MS.
 */
void sony898_parser_prepare_for_next_job(void);
