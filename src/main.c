/*
 * Demo: Sony UP-D898MD_X898MD USB printer emulator on ESP32-S3-N16R8.
 *
 * UART commands (115200 8N1):
 *   status    — parser state, image info, checksum
 *   dump_pgm  — emit raw PGM binary to UART (capture with e.g. minicom -b 115200 -C out.pgm)
 *   clear     — release image buffer, reset parser
 *   info      — device identity
 *   usb       — USB connection/configuration state
 */

#include "sony898_usb.h"
#include "sony898_parser.h"
#include "sony898_status.h"
#include "sony898_image.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "main";

#define UART_NUM     UART_NUM_0
#define UART_BUF_SZ  512
#define CMD_BUF_SZ   64

/* ── UART helpers ─────────────────────────────────────────────────────────── */

static void uart_puts(const char *s) {
    uart_write_bytes(UART_NUM, s, strlen(s));
}

static void uart_putline(const char *s) {
    uart_write_bytes(UART_NUM, s, strlen(s));
    uart_write_bytes(UART_NUM, "\r\n", 2);
}

/* ── Command handlers ─────────────────────────────────────────────────────── */

static const char *state_name(parser_state_t s) {
    switch (s) {
    case PARSER_WAIT_JOBSIZE_PJL_H: return "WAIT_JOBSIZE_PJL_H";
    case PARSER_READ_PJL_HEADER:    return "READ_PJL_HEADER";
    case PARSER_WAIT_JOBSIZE_PDL:   return "WAIT_JOBSIZE_PDL";
    case PARSER_READ_PDL_HEADER:    return "READ_PDL_HEADER";
    case PARSER_READ_IMAGE_PAYLOAD: return "READ_IMAGE_PAYLOAD";
    case PARSER_READ_PDL_FOOTER:    return "READ_PDL_FOOTER";
    case PARSER_WAIT_JOBSIZE_PJL_T: return "WAIT_JOBSIZE_PJL_T";
    case PARSER_READ_PJL_TRAILER:   return "READ_PJL_TRAILER";
    case PARSER_JOB_COMPLETE:       return "JOB_COMPLETE";
    case PARSER_ERROR:              return "ERROR";
    default:                        return "UNKNOWN";
    }
}

static void cmd_status(void) {
    char buf[128];
    snprintf(buf, sizeof(buf), "state: %s\r\n",
             state_name(sony898_parser_get_state()));
    uart_puts(buf);

    snprintf(buf, sizeof(buf), "image_ready: %d\r\n",
             (int)sony898_image_ready());
    uart_puts(buf);

    if (sony898_image_ready()) {
        snprintf(buf, sizeof(buf), "width: %"PRIu16"\r\n",
                 sony898_get_width());
        uart_puts(buf);
        snprintf(buf, sizeof(buf), "height: %"PRIu16"\r\n",
                 sony898_get_height());
        uart_puts(buf);
        snprintf(buf, sizeof(buf), "image_size: %zu bytes\r\n",
                 sony898_get_image_size());
        uart_puts(buf);

        /* compute checksum */
        const uint8_t *img = sony898_get_image_buffer();
        uint32_t sum = 0;
        size_t sz = sony898_get_image_size();
        for (size_t i = 0; i < sz; i++) sum += img[i];
        snprintf(buf, sizeof(buf), "checksum: 0x%08"PRIX32"\r\n", sum);
        uart_puts(buf);
    }
}

static void cmd_dump_pgm(void) {
    if (!sony898_image_ready()) {
        uart_putline("ERROR: no image");
        return;
    }
    uint16_t w = sony898_get_width();
    uint16_t h = sony898_get_height();
    const uint8_t *img = sony898_get_image_buffer();
    size_t sz = sony898_get_image_size();

    char hdr[64];
    int hdr_len = snprintf(hdr, sizeof(hdr),
                           "P5\r\n%"PRIu16" %"PRIu16"\r\n255\r\n", w, h);
    uart_write_bytes(UART_NUM, hdr, hdr_len);

    /* Send in chunks to avoid long non-yielding loops. */
    const size_t CHUNK = 4096;
    for (size_t offset = 0; offset < sz; offset += CHUNK) {
        size_t n = sz - offset;
        if (n > CHUNK) n = CHUNK;
        uart_write_bytes(UART_NUM, (const char *)(img + offset), n);
        taskYIELD();
    }
}

static void cmd_clear(void) {
    sony898_parser_reset();
    uart_putline("OK: image cleared, parser reset");
}

static void cmd_info(void) {
    uart_putline("VID:054C  PID:0877");
    uart_putline("Manufacturer : Sony");
    uart_putline("Product      : UP-D898MD_X898MD");
    uart_putline("Serial       : 0000000");
    uart_putline("Class        : 7/1/2 Printer Bidirectional");
    uart_putline("Protocol     : SPJL-DS, SPDL-DS2");
}

static void cmd_usb(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "connected=%d\r\n",
             (int)sony898_usb_is_connected());
    uart_puts(buf);
    snprintf(buf, sizeof(buf), "configured=%d\r\n",
             (int)sony898_usb_is_configured());
    uart_puts(buf);
    snprintf(buf, sizeof(buf), "ready_for_print=%d\r\n",
             (int)sony898_usb_is_ready_for_print());
    uart_puts(buf);
    snprintf(buf, sizeof(buf), "image_ready=%d\r\n",
             (int)sony898_image_ready());
    uart_puts(buf);
}

/* ── UART command task ────────────────────────────────────────────────────── */

static void uart_task(void *arg) {
    (void)arg;
    char cmd[CMD_BUF_SZ];
    int  cmd_len = 0;
    uint8_t ch;

    while (1) {
        int r = uart_read_bytes(UART_NUM, &ch, 1, pdMS_TO_TICKS(10));
        if (r <= 0) continue;

        if (ch == '\r' || ch == '\n') {
            if (cmd_len == 0) continue;
            cmd[cmd_len] = '\0';
            cmd_len = 0;
            uart_puts("\r\n");

            if      (strcmp(cmd, "status")   == 0) cmd_status();
            else if (strcmp(cmd, "dump_pgm") == 0) cmd_dump_pgm();
            else if (strcmp(cmd, "clear")    == 0) cmd_clear();
            else if (strcmp(cmd, "info")     == 0) cmd_info();
            else if (strcmp(cmd, "usb")      == 0) cmd_usb();
            else {
                uart_puts("unknown command: ");
                uart_putline(cmd);
                uart_putline("commands: status dump_pgm clear info usb");
            }
        } else if (ch == 0x08 || ch == 0x7F) {
            /* backspace */
            if (cmd_len > 0) cmd_len--;
        } else if (cmd_len < CMD_BUF_SZ - 1) {
            cmd[cmd_len++] = (char)ch;
        }
    }
}

/* ── Periodic status log ──────────────────────────────────────────────────── */

static void status_log_task(void *arg) {
    (void)arg;
    bool last_ready = false;
    bool last_connected = false;

    while (1) {
        bool connected = sony898_usb_is_connected();
        bool ready     = sony898_image_ready();

        if (connected != last_connected) {
            ESP_LOGI(TAG, "USB connected=%d configured=%d ready_for_print=%d",
                     connected,
                     sony898_usb_is_configured(),
                     sony898_usb_is_ready_for_print());
            last_connected = connected;
        }

        if (ready && !last_ready) {
            ESP_LOGI(TAG, "image_ready=1  %"PRIu16"x%"PRIu16"  %zu bytes",
                     sony898_get_width(), sony898_get_height(),
                     sony898_get_image_size());
        }
        last_ready = ready;

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ── app_main ─────────────────────────────────────────────────────────────── */

void app_main(void) {
    ESP_LOGI(TAG, "Sony UP-D898MD_X898MD emulator starting");

    /* UART */
    uart_config_t uart_cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, UART_BUF_SZ * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_cfg));

    /* Init modules */
    sony898_image_init();
    sony898_parser_init();
    sony898_status_init();

    /* USB */
    ESP_ERROR_CHECK(sony898_usb_init());

    sony898_usb_start_task();
    xTaskCreate(uart_task,       "uart",   4096, NULL, 5, NULL);
    xTaskCreate(status_log_task, "status", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "ready — connect USB cable and run: lp -d sony898 image.png");
    ESP_LOGI(TAG, "UART commands: status  dump_pgm  clear  info  usb");
}
