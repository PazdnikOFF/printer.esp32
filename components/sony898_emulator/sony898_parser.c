#include "sony898_parser.h"
#include "sony898_image.h"
#include "sony898_status.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "sony898_parser";

#define PDL_HDR_SIZE    CFG_PDL_HEADER_SIZE
#define PDL_FOOTER_SIZE CFG_PDL_FOOTER_SIZE
#define PDL_OFF_WIDTH   CFG_PDL_OFF_WIDTH
#define PDL_OFF_HEIGHT  CFG_PDL_OFF_HEIGHT

/*
 * Marker that ends the PJL header and immediately precedes binary PDL data.
 * Gutenprint sends the PJL block content directly over USB; the printer detects
 * this exact string to know where PDL binary data begins.
 */
static const char PJL_ENTER_MARKER[] = "@PJL ENTER LANGUAGE=SONY-PDL-DS2\r\n";
#define PJL_ENTER_LEN  (sizeof(PJL_ENTER_MARKER) - 1u)

typedef struct {
    parser_state_t state;

    /* byte-by-byte marker match position */
    uint32_t scan_pos;

    /* PDL binary header accumulator */
    uint8_t  pdl_header[PDL_HDR_SIZE];
    uint16_t pdl_header_pos;

    /* bytes remaining in current counted block */
    uint32_t block_remaining;

    /* image dimensions and write cursor */
    uint16_t width;
    uint16_t height;
    uint32_t image_written;

    /* copy count extracted from PDL header (default 1) */
    uint8_t copies;

    SemaphoreHandle_t lock;
} parser_ctx_t;

static parser_ctx_t _ctx;

/* ── helpers ─────────────────────────────────────────────────────────────── */

static inline uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void reset_locked(void) {
    _ctx.state           = PARSER_SCAN_PJL_HEADER;
    _ctx.scan_pos        = 0;
    _ctx.pdl_header_pos  = 0;
    _ctx.block_remaining = 0;
    _ctx.width           = 0;
    _ctx.height          = 0;
    _ctx.image_written   = 0;
    _ctx.copies          = 1;
    sony898_status_set_state(SONY898_STATE_IDLE);
}

/* ── PDL header handler ──────────────────────────────────────────────────── */

/* Pattern from driver: buf[i]==0x02, buf[i+1]==0x00, buf[i+2]==0x09 → copies at i+4 */
static uint8_t extract_copies(const uint8_t *hdr, size_t len) {
    for (size_t i = 0; i + 4 < len; i++) {
        if (hdr[i] == 0x02 && hdr[i+1] == 0x00 && hdr[i+2] == 0x09) {
            uint8_t n = hdr[i+4];
            return (n >= 1) ? n : 1;
        }
    }
    return 1;
}

static esp_err_t handle_pdl_header(void) {
    _ctx.width  = be16(_ctx.pdl_header + PDL_OFF_WIDTH);
    _ctx.height = be16(_ctx.pdl_header + PDL_OFF_HEIGHT);
    _ctx.copies = extract_copies(_ctx.pdl_header, PDL_HDR_SIZE);

    ESP_LOGI(TAG, "PDL header: %"PRIu16"x%"PRIu16" copies=%u",
             _ctx.width, _ctx.height, _ctx.copies);

    if (_ctx.width == 0 || _ctx.height == 0) {
        ESP_LOGE(TAG, "invalid dimensions %"PRIu16"x%"PRIu16,
                 _ctx.width, _ctx.height);
        return ESP_FAIL;
    }

    uint32_t image_bytes = (uint32_t)_ctx.width * _ctx.height;
    if (image_bytes > MAX_IMAGE_BYTES) {
        ESP_LOGE(TAG, "image too large: %"PRIu32" > %u", image_bytes, MAX_IMAGE_BYTES);
        return ESP_FAIL;
    }

    if (sony898_image_alloc(_ctx.width, _ctx.height) != ESP_OK) {
        return ESP_FAIL;
    }

    _ctx.image_written   = 0;
    _ctx.block_remaining = PDL_FOOTER_SIZE;
    _ctx.state           = PARSER_READ_IMAGE_PAYLOAD;
    return ESP_OK;
}

/* ── main feed function ──────────────────────────────────────────────────── */

static esp_err_t feed_locked(const uint8_t *data, size_t len) {
    while (len > 0) {
        switch (_ctx.state) {

        /* ── scan for "@PJL ENTER LANGUAGE=SONY-PDL-DS2\r\n" ────────────── */
        case PARSER_SCAN_PJL_HEADER: {
            /*
             * Byte-by-byte match.  Each mismatch resets the position,
             * with a fast-path for the common case where the failed byte
             * could restart the match.
             */
            while (len > 0) {
                uint8_t b = *data++; len--;
                if (b == (uint8_t)PJL_ENTER_MARKER[_ctx.scan_pos]) {
                    _ctx.scan_pos++;
                    if (_ctx.scan_pos == PJL_ENTER_LEN) {
                        ESP_LOGI(TAG, "PJL ENTER LANGUAGE found → reading PDL header");
                        _ctx.pdl_header_pos = 0;
                        _ctx.scan_pos = 0;
                        _ctx.state = PARSER_READ_PDL_HEADER;
                        sony898_status_set_state(SONY898_STATE_RECEIVING_JOB);
                        break;
                    }
                } else {
                    _ctx.scan_pos = 0;
                    if (b == (uint8_t)PJL_ENTER_MARKER[0]) _ctx.scan_pos = 1;
                }
            }
            break;
        }

        /* ── accumulate PDL binary header (290 bytes) ────────────────────── */
        case PARSER_READ_PDL_HEADER: {
            size_t n = PDL_HDR_SIZE - _ctx.pdl_header_pos;
            if (n > len) n = len;
            memcpy(_ctx.pdl_header + _ctx.pdl_header_pos, data, n);
            _ctx.pdl_header_pos += (uint16_t)n;
            data += n; len -= n;
            if (_ctx.pdl_header_pos == PDL_HDR_SIZE) {
                if (handle_pdl_header() != ESP_OK) {
                    _ctx.state = PARSER_ERROR;
                    return ESP_FAIL;
                }
            }
            break;
        }

        /* ── stream pixel data into PSRAM ────────────────────────────────── */
        case PARSER_READ_IMAGE_PAYLOAD: {
            uint32_t remaining = (uint32_t)_ctx.width * _ctx.height - _ctx.image_written;
            size_t n = (remaining < (uint32_t)len) ? (size_t)remaining : len;
            uint8_t *dst = sony898_image_get_write_ptr();
            if (!dst) {
                ESP_LOGE(TAG, "image write ptr NULL");
                _ctx.state = PARSER_ERROR;
                return ESP_FAIL;
            }
            memcpy(dst + _ctx.image_written, data, n);
            _ctx.image_written += (uint32_t)n;
            data += n; len -= n;
            if (_ctx.image_written == (uint32_t)_ctx.width * _ctx.height) {
                ESP_LOGI(TAG, "image complete: %"PRIu32" bytes", _ctx.image_written);
                sony898_status_set_state(SONY898_STATE_PRINTING);
                _ctx.state = PARSER_READ_PDL_FOOTER;
            }
            break;
        }

        /* ── discard PDL footer (7 bytes) ────────────────────────────────── */
        case PARSER_READ_PDL_FOOTER: {
            size_t n = (_ctx.block_remaining < (uint32_t)len) ?
                       (size_t)_ctx.block_remaining : len;
            _ctx.block_remaining -= (uint32_t)n;
            data += n; len -= n;
            if (_ctx.block_remaining == 0) {
                const uint8_t *img = sony898_image_get_write_ptr();
                if (img) {
                    uint32_t sum = 0;
                    size_t sz = sony898_get_image_size();
                    for (size_t i = 0; i < sz; i++) sum += img[i];
                    ESP_LOGI(TAG, "checksum = 0x%08"PRIX32, sum);
                }
                sony898_image_mark_ready();
                /* State: PRINTING → JOB_DONE transition is driven by
                 * status_log_task after CFG_PRINT_TIME_MS to simulate
                 * mechanical print time. */
                _ctx.state = PARSER_JOB_COMPLETE;
                ESP_LOGI(TAG, "PDL footer done — image in PSRAM, printing...");
            }
            break;
        }

        case PARSER_JOB_COMPLETE:
            data += len; len = 0;
            break;

        case PARSER_ERROR:
            data += len; len = 0;
            break;
        }
    }
    return ESP_OK;
}

/* ── public API ──────────────────────────────────────────────────────────── */

void sony898_parser_init(void) {
    _ctx.lock = xSemaphoreCreateMutex();
    configASSERT(_ctx.lock);
    reset_locked();
}

void sony898_parser_reset(void) {
    xSemaphoreTake(_ctx.lock, portMAX_DELAY);
    sony898_release_image();
    reset_locked();
    xSemaphoreGive(_ctx.lock);
    ESP_LOGI(TAG, "parser reset");
}

esp_err_t sony898_parser_feed(const uint8_t *data, size_t len) {
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(_ctx.lock, portMAX_DELAY);
    esp_err_t r = feed_locked(data, len);
    xSemaphoreGive(_ctx.lock);
    return r;
}

bool sony898_parser_can_accept_job(void) {
    return _ctx.state == PARSER_SCAN_PJL_HEADER;
}

parser_state_t sony898_parser_get_state(void) {
    return _ctx.state;
}

uint8_t sony898_parser_get_copies(void) {
    return (_ctx.copies >= 1) ? _ctx.copies : 1;
}

void sony898_parser_prepare_for_next_job(void) {
    xSemaphoreTake(_ctx.lock, portMAX_DELAY);
    _ctx.state           = PARSER_SCAN_PJL_HEADER;
    _ctx.scan_pos        = 0;
    _ctx.pdl_header_pos  = 0;
    _ctx.block_remaining = 0;
    _ctx.width           = 0;
    _ctx.height          = 0;
    _ctx.image_written   = 0;
    _ctx.copies          = 1;
    sony898_status_set_state(SONY898_STATE_IDLE);
    xSemaphoreGive(_ctx.lock);
    ESP_LOGI(TAG, "ready for next job (image retained in PSRAM)");
}
