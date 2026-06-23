#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    PARSER_WAIT_JOBSIZE_PJL_H = 0,
    PARSER_READ_PJL_HEADER,
    PARSER_WAIT_JOBSIZE_PDL,
    PARSER_READ_PDL_HEADER,
    PARSER_READ_IMAGE_PAYLOAD,
    PARSER_READ_PDL_FOOTER,
    PARSER_WAIT_JOBSIZE_PJL_T,
    PARSER_READ_PJL_TRAILER,
    PARSER_JOB_COMPLETE,
    PARSER_ERROR,
} parser_state_t;

void        sony898_parser_init(void);
void        sony898_parser_reset(void);
esp_err_t   sony898_parser_feed(const uint8_t *data, size_t len);
bool        sony898_parser_can_accept_job(void);
parser_state_t sony898_parser_get_state(void);
