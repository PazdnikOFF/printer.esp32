#include "sony898_parser.h"
#include "sony898_image.h"
#include "sony898_status.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "sony898_parser";

/* JOBSIZE block is exactly 256 bytes, zero-padded. */
#define JOBSIZE_BLOCK  256u
/* PDL header size confirmed by ТЗ §6. */
#define PDL_HDR_SIZE   290u
/* PDL footer size confirmed by ТЗ §6. */
#define PDL_FOOTER_SIZE 7u
/* Offsets within PDL header confirmed by ТЗ §6. */
#define PDL_OFF_WIDTH   0x24u
#define PDL_OFF_HEIGHT  0x26u

/*
 * PJL header / trailer scanned to log protocol landmarks.
 * Buffering limited to SCAN_BUF_SIZE to avoid internal-RAM allocation
 * for variable-length PJL blocks.  Remainder is skipped without copy.
 */
#define SCAN_BUF_SIZE  512u

typedef struct {
    parser_state_t state;

    uint8_t  jobsize_buf[JOBSIZE_BLOCK];
    uint16_t jobsize_pos;

    uint8_t  pdl_header[PDL_HDR_SIZE];
    uint16_t pdl_header_pos;

    /* scan buffer for PJL header / trailer landmarks */
    uint8_t  scan_buf[SCAN_BUF_SIZE];
    uint16_t scan_pos;

    /* bytes remaining in the current block */
    uint32_t block_remaining;

    /* parsed block sizes */
    uint32_t pjl_h_len;
    uint32_t pdl_len;
    uint32_t pjl_t_len;

    /* image state */
    uint16_t width;
    uint16_t height;
    uint32_t image_written;

    SemaphoreHandle_t lock;
} parser_ctx_t;

static parser_ctx_t _ctx;

/* ── helpers ─────────────────────────────────────────────────────────────── */

static inline uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void reset_locked(void) {
    _ctx.state         = PARSER_WAIT_JOBSIZE_PJL_H;
    _ctx.jobsize_pos   = 0;
    _ctx.pdl_header_pos = 0;
    _ctx.scan_pos      = 0;
    _ctx.block_remaining = 0;
    _ctx.pjl_h_len     = 0;
    _ctx.pdl_len       = 0;
    _ctx.pjl_t_len     = 0;
    _ctx.width         = 0;
    _ctx.height        = 0;
    _ctx.image_written = 0;
    sony898_status_set_state(SONY898_STATE_IDLE);
}

/* ── JOBSIZE block parser ────────────────────────────────────────────────── */

static void handle_jobsize(void) {
    const char *buf = (const char *)_ctx.jobsize_buf;

    if (strncmp(buf, "JOBSIZE=", 8) != 0) {
        ESP_LOGE(TAG, "expected JOBSIZE=, got: %.32s", buf);
        _ctx.state = PARSER_ERROR;
        return;
    }

    const char *p = buf + 8;

    if (_ctx.state == PARSER_WAIT_JOBSIZE_PJL_H) {
        if (strncmp(p, "PJL-H,", 6) != 0) {
            ESP_LOGE(TAG, "expected PJL-H, got: %.32s", p);
            _ctx.state = PARSER_ERROR;
            return;
        }
        _ctx.pjl_h_len = (uint32_t)atoi(p + 6);
        ESP_LOGI(TAG, "found JOBSIZE=PJL-H,%"PRIu32, _ctx.pjl_h_len);
        _ctx.block_remaining = _ctx.pjl_h_len;
        _ctx.scan_pos = 0;
        _ctx.state = PARSER_READ_PJL_HEADER;
        sony898_status_set_state(SONY898_STATE_RECEIVING_JOB);

    } else if (_ctx.state == PARSER_WAIT_JOBSIZE_PDL) {
        if (strncmp(p, "PDL,", 4) != 0) {
            ESP_LOGE(TAG, "expected PDL, got: %.32s", p);
            _ctx.state = PARSER_ERROR;
            return;
        }
        _ctx.pdl_len = (uint32_t)atoi(p + 4);
        ESP_LOGI(TAG, "found JOBSIZE=PDL,%"PRIu32, _ctx.pdl_len);
        _ctx.pdl_header_pos = 0;
        _ctx.state = PARSER_READ_PDL_HEADER;

    } else if (_ctx.state == PARSER_WAIT_JOBSIZE_PJL_T) {
        if (strncmp(p, "PJL-T,", 6) != 0) {
            ESP_LOGE(TAG, "expected PJL-T, got: %.32s", p);
            _ctx.state = PARSER_ERROR;
            return;
        }
        _ctx.pjl_t_len = (uint32_t)atoi(p + 6);
        ESP_LOGI(TAG, "found JOBSIZE=PJL-T,%"PRIu32, _ctx.pjl_t_len);
        _ctx.block_remaining = _ctx.pjl_t_len;
        _ctx.scan_pos = 0;
        _ctx.state = PARSER_READ_PJL_TRAILER;

    } else {
        ESP_LOGE(TAG, "unexpected JOBSIZE in state %d", (int)_ctx.state);
        _ctx.state = PARSER_ERROR;
    }
}

/* ── PDL header parser ───────────────────────────────────────────────────── */

static void handle_pdl_header(void) {
    _ctx.width  = be16(_ctx.pdl_header + PDL_OFF_WIDTH);
    _ctx.height = be16(_ctx.pdl_header + PDL_OFF_HEIGHT);

    ESP_LOGI(TAG, "PDL header: width=%"PRIu16" height=%"PRIu16,
             _ctx.width, _ctx.height);

    if (_ctx.width == 0 || _ctx.height == 0) {
        ESP_LOGE(TAG, "invalid image dimensions %"PRIu16"x%"PRIu16,
                 _ctx.width, _ctx.height);
        _ctx.state = PARSER_ERROR;
        return;
    }

    uint32_t image_bytes = (uint32_t)_ctx.width * _ctx.height;

    if (image_bytes > MAX_IMAGE_BYTES) {
        ESP_LOGE(TAG, "image too large: %"PRIu32" > %u", image_bytes, MAX_IMAGE_BYTES);
        _ctx.state = PARSER_ERROR;
        return;
    }

    uint32_t expected_pdl = PDL_HDR_SIZE + image_bytes + PDL_FOOTER_SIZE;
    if (_ctx.pdl_len != expected_pdl) {
        ESP_LOGE(TAG, "PDL length mismatch: got %"PRIu32" expected %"PRIu32,
                 _ctx.pdl_len, expected_pdl);
        _ctx.state = PARSER_ERROR;
        return;
    }

    ESP_LOGI(TAG, "PDL header size = %u", PDL_HDR_SIZE);
    ESP_LOGI(TAG, "image payload = %"PRIu32" bytes", image_bytes);
    ESP_LOGI(TAG, "PDL footer = %u bytes", PDL_FOOTER_SIZE);

    if (sony898_image_alloc(_ctx.width, _ctx.height) != ESP_OK) {
        _ctx.state = PARSER_ERROR;
        return;
    }

    _ctx.image_written = 0;
    _ctx.state = PARSER_READ_IMAGE_PAYLOAD;
}

/* ── PJL scan helper: look for a string landmark ────────────────────────── */

static void scan_for(const char *landmark, const char *label) {
    /* scan_buf holds a prefix of the PJL block for landmark detection */
    if (memmem(_ctx.scan_buf, _ctx.scan_pos, landmark, strlen(landmark))) {
        ESP_LOGI(TAG, "found %s", label);
    }
}

/* ── main feed function ──────────────────────────────────────────────────── */

static esp_err_t feed_locked(const uint8_t *data, size_t len) {
    while (len > 0) {
        switch (_ctx.state) {

        /* ── accumulate 256-byte JOBSIZE block ───────────────────────────── */
        case PARSER_WAIT_JOBSIZE_PJL_H:
        case PARSER_WAIT_JOBSIZE_PDL:
        case PARSER_WAIT_JOBSIZE_PJL_T: {
            size_t n = JOBSIZE_BLOCK - _ctx.jobsize_pos;
            if (n > len) n = len;
            memcpy(_ctx.jobsize_buf + _ctx.jobsize_pos, data, n);
            _ctx.jobsize_pos += n;
            data += n; len -= n;
            if (_ctx.jobsize_pos == JOBSIZE_BLOCK) {
                _ctx.jobsize_pos = 0;
                handle_jobsize();
                if (_ctx.state == PARSER_ERROR) return ESP_FAIL;
            }
            break;
        }

        /* ── read + scan PJL header ──────────────────────────────────────── */
        case PARSER_READ_PJL_HEADER: {
            /* fill scan buffer first (limited to SCAN_BUF_SIZE) */
            if (_ctx.scan_pos < SCAN_BUF_SIZE) {
                size_t room = SCAN_BUF_SIZE - _ctx.scan_pos;
                size_t n = _ctx.block_remaining < room ? _ctx.block_remaining : room;
                if (n > len) n = len;
                memcpy(_ctx.scan_buf + _ctx.scan_pos, data, n);
                _ctx.scan_pos += n;
                _ctx.block_remaining -= n;
                data += n; len -= n;
            } else {
                /* skip the rest */
                size_t n = _ctx.block_remaining < len ? _ctx.block_remaining : len;
                _ctx.block_remaining -= n;
                data += n; len -= n;
            }
            if (_ctx.block_remaining == 0) {
                scan_for("@PJL ENTER LANGUAGE=SONY-PDL-DS2",
                         "@PJL ENTER LANGUAGE=SONY-PDL-DS2");
                _ctx.jobsize_pos = 0;
                _ctx.state = PARSER_WAIT_JOBSIZE_PDL;
            }
            break;
        }

        /* ── accumulate PDL header (290 bytes) ───────────────────────────── */
        case PARSER_READ_PDL_HEADER: {
            size_t n = PDL_HDR_SIZE - _ctx.pdl_header_pos;
            if (n > len) n = len;
            memcpy(_ctx.pdl_header + _ctx.pdl_header_pos, data, n);
            _ctx.pdl_header_pos += n;
            data += n; len -= n;
            if (_ctx.pdl_header_pos == PDL_HDR_SIZE) {
                handle_pdl_header();
                if (_ctx.state == PARSER_ERROR) return ESP_FAIL;
            }
            break;
        }

        /* ── stream image payload into PSRAM ─────────────────────────────── */
        case PARSER_READ_IMAGE_PAYLOAD: {
            uint32_t remaining = (uint32_t)_ctx.width * _ctx.height - _ctx.image_written;
            size_t n = remaining < len ? remaining : len;
            uint8_t *dst = sony898_image_get_write_ptr();
            if (!dst) {
                ESP_LOGE(TAG, "image write ptr is NULL");
                _ctx.state = PARSER_ERROR;
                return ESP_FAIL;
            }
            memcpy(dst + _ctx.image_written, data, n);
            _ctx.image_written += n;
            data += n; len -= n;
            if (_ctx.image_written == (uint32_t)_ctx.width * _ctx.height) {
                ESP_LOGI(TAG, "image size = %"PRIu32" bytes", _ctx.image_written);
                _ctx.block_remaining = PDL_FOOTER_SIZE;
                _ctx.state = PARSER_READ_PDL_FOOTER;
            }
            break;
        }

        /* ── skip PDL footer (7 bytes) ───────────────────────────────────── */
        case PARSER_READ_PDL_FOOTER: {
            size_t n = _ctx.block_remaining < len ? _ctx.block_remaining : len;
            _ctx.block_remaining -= n;
            data += n; len -= n;
            if (_ctx.block_remaining == 0) {
                _ctx.jobsize_pos = 0;
                _ctx.state = PARSER_WAIT_JOBSIZE_PJL_T;
            }
            break;
        }

        /* ── read + scan PJL trailer ─────────────────────────────────────── */
        case PARSER_READ_PJL_TRAILER: {
            if (_ctx.scan_pos < SCAN_BUF_SIZE) {
                size_t room = SCAN_BUF_SIZE - _ctx.scan_pos;
                size_t n = _ctx.block_remaining < room ? _ctx.block_remaining : room;
                if (n > len) n = len;
                memcpy(_ctx.scan_buf + _ctx.scan_pos, data, n);
                _ctx.scan_pos += n;
                _ctx.block_remaining -= n;
                data += n; len -= n;
            } else {
                size_t n = _ctx.block_remaining < len ? _ctx.block_remaining : len;
                _ctx.block_remaining -= n;
                data += n; len -= n;
            }
            if (_ctx.block_remaining == 0) {
                scan_for("@PJL EOJ", "@PJL EOJ");
                /* Log computed checksum for verification */
                const uint8_t *img = sony898_image_get_write_ptr();
                if (img) {
                    uint32_t sum = 0;
                    size_t sz = sony898_get_image_size();
                    for (size_t i = 0; i < sz; i++) sum += img[i];
                    ESP_LOGI(TAG, "checksum = 0x%08"PRIX32, sum);
                }
                sony898_image_mark_ready();
                ESP_LOGI(TAG, "image_ready = 1");
                sony898_status_set_state(SONY898_STATE_JOB_DONE);
                _ctx.state = PARSER_JOB_COMPLETE;
            }
            break;
        }

        case PARSER_JOB_COMPLETE:
            /* consume and discard — host may send trailing bytes */
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
    return _ctx.state == PARSER_WAIT_JOBSIZE_PJL_H;
}

parser_state_t sony898_parser_get_state(void) {
    return _ctx.state;
}
