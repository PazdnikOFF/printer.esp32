/*
 * Sony UP-D898MD_X898MD USB printer emulator — application entry point.
 *
 * This file contains only configuration, wiring and the optional UART
 * service interface.  All emulator logic lives in the sony898_emulator
 * component and is accessed through its public API.
 *
 * ── UART service commands (115200 8N1) ───────────────────────────────────────
 *   status              — printer / USB / image state
 *   device_id           — current IEEE1284 Device ID string
 *   serial              — active serial number
 *   counter             — total print count
 *   dump_pgm            — emit raw PGM image over UART
 *   clear               — release image buffer and reset parser
 *   info                — USB identity
 *   set_serial <value>  — write serial to NVS (factory, reboot to apply)
 *   set <state>         — force printer state for testing
 */

#include "sony898_emulator.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

static const char *TAG = "main";

#define UART_NUM    UART_NUM_0
#define UART_BUF_SZ 512
#define CMD_BUF_SZ  64

/* ── UART helpers ────────────────────────────────────────────────────────────*/

static void uart_puts(const char *s) {
    uart_write_bytes(UART_NUM, s, strlen(s));
}

static void uart_putline(const char *s) {
    uart_write_bytes(UART_NUM, s, strlen(s));
    uart_write_bytes(UART_NUM, "\r\n", 2);
}

static void uart_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    uart_puts(buf);
}

/* ── Service UART commands ───────────────────────────────────────────────────*/

static void cmd_status(void) {
    uart_printf("state      : %s\r\n",   sony898_emulator_get_state_name());
    uart_printf("port_status: 0x%02X\r\n", sony898_emulator_get_port_status());
    uart_printf("usb        : connected=%d  configured=%d  ready=%d\r\n",
                (int)sony898_emulator_usb_is_connected(),
                (int)sony898_emulator_usb_is_configured(),
                (int)sony898_emulator_usb_is_ready());
    uart_printf("image      : ready=%d",  (int)sony898_emulator_image_ready());
    if (sony898_emulator_image_ready()) {
        uart_printf("  %"PRIu16"x%"PRIu16"  %zu B  copies=%u",
                    sony898_emulator_get_width(),
                    sony898_emulator_get_height(),
                    sony898_emulator_get_image_size(),
                    sony898_emulator_get_copies());
    }
    uart_puts("\r\n");
    uart_printf("print_mod  : %s\r\n",
                sony898_emulator_has_print_module() ? "registered" : "timer mode");
    uart_printf("sensors_ok : %d\r\n",
                (int)!sony898_emulator_has_sensor_error());
    uart_printf("counter    : %"PRIu32"\r\n", sony898_emulator_get_print_count());
}

static void cmd_device_id(void) {
    uart_putline(sony898_emulator_get_ieee1284_id());
}

static void cmd_usb(void) {
    uart_printf("connected  : %d\r\n", (int)sony898_emulator_usb_is_connected());
    uart_printf("configured : %d\r\n", (int)sony898_emulator_usb_is_configured());
    uart_printf("ready      : %d\r\n", (int)sony898_emulator_usb_is_ready());
}

static void cmd_serial(void) {
    uart_printf("serial: %s\r\n", sony898_emulator_get_serial());
}

static void cmd_set_serial(const char *arg) {
    if (!arg || !arg[0]) {
        uart_putline("usage: set_serial <value>  (max 16 chars)");
        return;
    }
    esp_err_t r = sony898_emulator_set_serial(arg);
    if (r == ESP_OK) {
        uart_printf("OK: saved — reboot to apply: %s\r\n", arg);
    } else {
        uart_putline("ERROR: NVS write failed");
    }
}

static void cmd_counter(void) {
    uart_printf("print_count: %"PRIu32"\r\n", sony898_emulator_get_print_count());
}

static void cmd_info(void) {
    uart_putline("VID          : 054C");
    uart_putline("PID          : 0877");
    uart_putline("Manufacturer : Sony");
    uart_putline("Product      : UP-D898MD_X898MD");
    uart_printf("Serial       : %s\r\n", sony898_emulator_get_serial());
    uart_putline("Class        : 7/1/2 Printer Bidirectional");
    uart_putline("Protocol     : SPJL-DS, SPDL-DS2");
}

static void cmd_dump_pgm(void) {
    if (!sony898_emulator_image_ready()) {
        uart_putline("ERROR: no image");
        return;
    }
    uint16_t w  = sony898_emulator_get_width();
    uint16_t h  = sony898_emulator_get_height();
    size_t   sz = sony898_emulator_get_image_size();
    const uint8_t *img = sony898_emulator_get_image_buffer();

    char hdr[64];
    int hlen = snprintf(hdr, sizeof(hdr), "P5\r\n%"PRIu16" %"PRIu16"\r\n255\r\n", w, h);
    uart_write_bytes(UART_NUM, hdr, hlen);

    const size_t CHUNK = 4096;
    for (size_t off = 0; off < sz; off += CHUNK) {
        size_t n = sz - off;
        if (n > CHUNK) n = CHUNK;
        uart_write_bytes(UART_NUM, (const char *)(img + off), n);
        taskYIELD();
    }
}

static void cmd_clear(void) {
    sony898_emulator_clear_job();
    uart_putline("OK: image cleared, parser reset");
}

static void cmd_set_state(const char *arg) {
    sony898_state_t s;
    if      (strcmp(arg, "idle")           == 0) s = SONY898_STATE_IDLE;
    else if (strcmp(arg, "receiving_job")  == 0) s = SONY898_STATE_RECEIVING_JOB;
    else if (strcmp(arg, "printing")       == 0) s = SONY898_STATE_PRINTING;
    else if (strcmp(arg, "job_done")       == 0) s = SONY898_STATE_JOB_DONE;
    else if (strcmp(arg, "cover_open")     == 0) s = SONY898_STATE_COVER_OPEN;
    else if (strcmp(arg, "no_paper")       == 0) s = SONY898_STATE_NO_PAPER;
    else if (strcmp(arg, "no_ribbon")      == 0) s = SONY898_STATE_NO_RIBBON;
    else if (strcmp(arg, "no_media")       == 0) s = SONY898_STATE_NO_MEDIA;
    else if (strcmp(arg, "media_mismatch") == 0) s = SONY898_STATE_MEDIA_MISMATCH;
    else if (strcmp(arg, "error")          == 0) s = SONY898_STATE_SYSTEM_ERROR;
    else {
        uart_printf("unknown state: %s\r\n", arg);
        uart_putline("states: idle receiving_job printing job_done cover_open"
                     " no_paper no_ribbon no_media media_mismatch error");
        return;
    }
    sony898_emulator_set_state(s);
    uart_printf("state → %s\r\n", sony898_emulator_get_state_name());
}

/* ── UART task ───────────────────────────────────────────────────────────────*/

static void uart_task(void *arg) {
    (void)arg;
    char    cmd[CMD_BUF_SZ];
    int     cmd_len = 0;
    uint8_t ch;

    while (1) {
        if (uart_read_bytes(UART_NUM, &ch, 1, pdMS_TO_TICKS(10)) <= 0) continue;

        if (ch == '\r' || ch == '\n') {
            if (cmd_len == 0) continue;
            cmd[cmd_len] = '\0';
            cmd_len = 0;
            uart_puts("\r\n");

            if      (strcmp(cmd, "status")           == 0) cmd_status();
            else if (strcmp(cmd, "device_id")        == 0) cmd_device_id();
            else if (strcmp(cmd, "usb")              == 0) cmd_usb();
            else if (strcmp(cmd, "serial")           == 0) cmd_serial();
            else if (strcmp(cmd, "counter")          == 0) cmd_counter();
            else if (strcmp(cmd, "dump_pgm")         == 0) cmd_dump_pgm();
            else if (strcmp(cmd, "clear")            == 0) cmd_clear();
            else if (strcmp(cmd, "info")             == 0) cmd_info();
            else if (strncmp(cmd, "set_serial ", 11) == 0) cmd_set_serial(cmd + 11);
            else if (strncmp(cmd, "set ", 4)         == 0) cmd_set_state(cmd + 4);
            else {
                uart_printf("unknown: %s\r\n", cmd);
                uart_putline("commands: status | device_id | usb | serial | counter |"
                             " dump_pgm | clear | info | set_serial <v> | set <state>");
            }
        } else if ((ch == 0x08 || ch == 0x7F) && cmd_len > 0) {
            cmd_len--;
        } else if (cmd_len < CMD_BUF_SZ - 1) {
            cmd[cmd_len++] = (char)ch;
        }
    }
}

/* ── Print module wiring ─────────────────────────────────────────────────────
 *
 * Wire the thermal print module callback and optional USB event handlers here.
 *
 * The thermal module must:
 *   1. Implement the job callback and assign it to cfg->on_job_ready.
 *   2. Report sensor states from GPIO handlers:
 *        sony898_emulator_set_paper(gpio_get_level(PIN_PAPER));
 *        sony898_emulator_set_cover(gpio_get_level(PIN_COVER));
 *        sony898_emulator_set_ribbon(gpio_get_level(PIN_RIBBON));
 *        sony898_emulator_set_media_match(media_ok);
 *        sony898_emulator_set_system_error(hw_fault);
 *   3. Signal completion from its print task:
 *        sony898_emulator_notify_done();    // success
 *        sony898_emulator_notify_error();   // hardware fault
 * ────────────────────────────────────────────────────────────────────────────*/

static void printer_module_init(sony898_config_t *cfg) {
    (void)cfg;
    /* TODO: wire thermal print module
     *   thermal_printer_init();
     *   cfg->on_job_ready = thermal_printer_on_job_ready;
     */
}

/* ── app_main ────────────────────────────────────────────────────────────────*/

void app_main(void) {
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    uart_config_t uart_cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, UART_BUF_SZ * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_cfg));

    sony898_config_t cfg = {
        .serial        = NULL,  /* load from NVS; program with: set_serial <value> */
        .on_job_ready  = NULL,
        .on_connect    = NULL,
        .on_disconnect = NULL,
    };
    printer_module_init(&cfg);

    ESP_ERROR_CHECK(sony898_emulator_init(&cfg));

    xTaskCreate(uart_task, "uart", 4096, NULL, 5, NULL);

    sony898_emulator_start();

    ESP_LOGI(TAG, "ready — serial: %s", sony898_emulator_get_serial());
}
