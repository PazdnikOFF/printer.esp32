/*
 * Sony UP-D898MD_X898MD USB printer emulator — application entry point.
 *
 * This module receives print jobs from a USB host (Linux/CUPS + Gutenprint),
 * parses SPJL-DS / SPDL-DS2, stores the grayscale image in PSRAM, and
 * exposes it to the thermal print module via sony898_printer_iface.h.
 *
 * ── Module wiring (in app_main) ──────────────────────────────────────────────
 *
 *   [USB host] → sony898_emulator → on_job_ready_cb → [thermal print module]
 *                                                             │
 *   sony898_sensor_set_*(...)  ←──────────────────── GPIO sensors
 *   sony898_printer_notify_done() ←──────────────── print complete signal
 *
 * ── UART service commands (115200 8N1) ───────────────────────────────────────
 *   status      — printer state, parser state, image info
 *   device_id   — current IEEE1284 Device ID string
 *   usb         — USB connection / configuration state
 *   counter     — total print count (NVS-persistent)
 *   dump_pgm    — emit raw PGM binary over UART
 *   clear       — release image buffer, reset parser
 *   info        — USB device identity
 *   set <state> — force printer state for testing:
 *                   idle / receiving_job / printing / job_done /
 *                   cover_open / no_paper / no_ribbon / no_media /
 *                   media_mismatch / error
 */

#include "sony898_usb.h"
#include "sony898_parser.h"
#include "sony898_status.h"
#include "sony898_image.h"
#include "sony898_sensors.h"
#include "sony898_printer_iface.h"
#include "sony898_counter.h"
#include "config.h"

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

/* ── Service UART commands ───────────────────────────────────────────────────*/

static const char *parser_state_name(parser_state_t s) {
    switch (s) {
    case PARSER_SCAN_PJL_HEADER:    return "SCAN_PJL_HEADER";
    case PARSER_READ_PDL_HEADER:    return "READ_PDL_HEADER";
    case PARSER_READ_IMAGE_PAYLOAD: return "READ_IMAGE_PAYLOAD";
    case PARSER_READ_PDL_FOOTER:    return "READ_PDL_FOOTER";
    case PARSER_JOB_COMPLETE:       return "JOB_COMPLETE";
    case PARSER_ERROR:              return "ERROR";
    default:                        return "UNKNOWN";
    }
}

static void cmd_status(void) {
    char buf[128];
    snprintf(buf, sizeof(buf), "printer_state : %s\r\n",
             sony898_status_state_name(sony898_status_get_state()));
    uart_puts(buf);
    snprintf(buf, sizeof(buf), "is_ready      : %d  port_status: 0x%02X\r\n",
             (int)sony898_status_is_ready(),
             sony898_status_get_port_status());
    uart_puts(buf);
    snprintf(buf, sizeof(buf), "parser        : %s\r\n",
             parser_state_name(sony898_parser_get_state()));
    uart_puts(buf);
    snprintf(buf, sizeof(buf), "image_ready   : %d\r\n",
             (int)sony898_image_ready());
    uart_puts(buf);
    snprintf(buf, sizeof(buf), "print_module  : %s\r\n",
             sony898_printer_iface_has_module() ? "registered" : "none (timer mode)");
    uart_puts(buf);

    if (sony898_image_ready()) {
        snprintf(buf, sizeof(buf), "width         : %"PRIu16"\r\n", sony898_get_width());
        uart_puts(buf);
        snprintf(buf, sizeof(buf), "height        : %"PRIu16"\r\n", sony898_get_height());
        uart_puts(buf);
        snprintf(buf, sizeof(buf), "size          : %zu bytes\r\n", sony898_get_image_size());
        uart_puts(buf);
        snprintf(buf, sizeof(buf), "copies        : %u\r\n", sony898_parser_get_copies());
        uart_puts(buf);

        const uint8_t *img = sony898_get_image_buffer();
        uint32_t sum = 0;
        size_t sz = sony898_get_image_size();
        for (size_t i = 0; i < sz; i++) sum += img[i];
        snprintf(buf, sizeof(buf), "checksum      : 0x%08"PRIX32"\r\n", sum);
        uart_puts(buf);
    }
}

static void cmd_device_id(void) {
    uart_putline(sony898_status_get_ieee1284_id());
}

static void cmd_usb(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "connected     : %d\r\n", (int)sony898_usb_is_connected());
    uart_puts(buf);
    snprintf(buf, sizeof(buf), "configured    : %d\r\n", (int)sony898_usb_is_configured());
    uart_puts(buf);
    snprintf(buf, sizeof(buf), "ready_for_print: %d\r\n", (int)sony898_usb_is_ready_for_print());
    uart_puts(buf);
    snprintf(buf, sizeof(buf), "image_ready   : %d\r\n", (int)sony898_image_ready());
    uart_puts(buf);
}

static void cmd_counter(void) {
    char buf[48];
    snprintf(buf, sizeof(buf), "print_count: %"PRIu32"\r\n", sony898_counter_get());
    uart_puts(buf);
}

static void cmd_info(void) {
    uart_putline("VID          : 054C");
    uart_putline("PID          : 0877");
    uart_putline("Manufacturer : Sony");
    uart_putline("Product      : UP-D898MD_X898MD");
    uart_putline("Class        : 7/1/2 Printer Bidirectional");
    uart_putline("Protocol     : SPJL-DS, SPDL-DS2");
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

/* Force printer state — for testing only, bypasses sensor logic. */
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
        uart_puts("unknown state: "); uart_putline(arg);
        uart_putline("states: idle receiving_job printing job_done cover_open"
                     " no_paper no_ribbon no_media media_mismatch error");
        return;
    }
    sony898_status_set_state(s);
    uart_puts("state → ");
    uart_putline(sony898_status_state_name(s));
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

            if      (strcmp(cmd, "status")    == 0) cmd_status();
            else if (strcmp(cmd, "device_id") == 0) cmd_device_id();
            else if (strcmp(cmd, "usb")       == 0) cmd_usb();
            else if (strcmp(cmd, "counter")   == 0) cmd_counter();
            else if (strcmp(cmd, "dump_pgm")  == 0) cmd_dump_pgm();
            else if (strcmp(cmd, "clear")     == 0) cmd_clear();
            else if (strcmp(cmd, "info")      == 0) cmd_info();
            else if (strncmp(cmd, "set ", 4)  == 0) cmd_set_state(cmd + 4);
            else {
                uart_puts("unknown: "); uart_putline(cmd);
                uart_putline("commands: status | device_id | usb | counter |"
                             " dump_pgm | clear | info | set <state>");
            }
        } else if ((ch == 0x08 || ch == 0x7F) && cmd_len > 0) {
            cmd_len--;
        } else if (cmd_len < CMD_BUF_SZ - 1) {
            cmd[cmd_len++] = (char)ch;
        }
    }
}

/* ── Status / dispatch task ──────────────────────────────────────────────────*/

static void status_log_task(void *arg) {
    (void)arg;
    bool       last_connected = false;
    bool       last_ready     = false;
    TickType_t printing_since = 0;
    TickType_t job_done_at    = 0;
    uint32_t   print_time_ms  = 0;
    bool       job_dispatched = false;

    while (1) {
        bool connected = sony898_usb_is_connected();
        if (connected != last_connected) {
            if (connected) {
                ESP_LOGI(TAG, "USB host connected — configured=%d ready_for_print=%d",
                         (int)sony898_usb_is_configured(),
                         (int)sony898_usb_is_ready_for_print());
            } else {
                ESP_LOGW(TAG, "USB host disconnected — printer_state=%s",
                         sony898_status_state_name(sony898_status_get_state()));
            }
            last_connected = connected;
        }

        bool ready = sony898_image_ready();

        if (ready && !last_ready) {
            ESP_LOGI(TAG, "image_ready  %"PRIu16"x%"PRIu16"  %zu B  copies=%u",
                     sony898_get_width(), sony898_get_height(),
                     sony898_get_image_size(), sony898_parser_get_copies());
        }
        last_ready = ready;

        sony898_state_t pstate = sony898_status_get_state();

        if (pstate == SONY898_STATE_PRINTING) {

            if (!job_dispatched) {
                job_dispatched = true;

                if (sony898_printer_iface_has_module()) {
                    sony898_print_job_t job = {
                        .buf    = sony898_get_image_buffer(),
                        .width  = sony898_get_width(),
                        .height = sony898_get_height(),
                        .size   = sony898_get_image_size(),
                        .copies = sony898_parser_get_copies(),
                    };
                    sony898_printer_iface_dispatch(&job);
                } else {
                    /* Fallback: simulate print time from image height */
                    printing_since = xTaskGetTickCount();
                    uint16_t h = sony898_get_height();
                    if (h == 0) h = 960;
                    print_time_ms = ((uint32_t)h * 1000u) / CFG_PRINT_SPEED_LPS;
                    ESP_LOGI(TAG, "print timer: %"PRIu16" lines → %"PRIu32" ms",
                             h, print_time_ms);
                }
            }

            if (sony898_printer_iface_check_error()) {
                sony898_status_set_state(SONY898_STATE_SYSTEM_ERROR);
                job_dispatched = false;
            } else if (sony898_printer_iface_check_done()) {
                sony898_status_set_state(SONY898_STATE_JOB_DONE);
                job_dispatched = false;
            } else if (!sony898_printer_iface_has_module() && printing_since &&
                       (xTaskGetTickCount() - printing_since) >= pdMS_TO_TICKS(print_time_ms)) {
                sony898_status_set_state(SONY898_STATE_JOB_DONE);
                printing_since = 0;
                job_dispatched = false;
            }
            job_done_at = 0;

        } else if (pstate == SONY898_STATE_JOB_DONE) {
            job_dispatched = false;
            printing_since = 0;
            if (job_done_at == 0) {
                job_done_at = xTaskGetTickCount();
            } else if ((xTaskGetTickCount() - job_done_at) >=
                       pdMS_TO_TICKS(CFG_JOB_DONE_IDLE_DELAY_MS)) {
                sony898_counter_increment();
                sony898_parser_prepare_for_next_job();
                job_done_at = 0;
            }
        } else {
            job_dispatched = false;
            printing_since = 0;
            job_done_at    = 0;
        }

        /* Poll faster when waiting for print dispatch or completion signal. */
        TickType_t delay = (pstate == SONY898_STATE_PRINTING)
                           ? pdMS_TO_TICKS(50)
                           : pdMS_TO_TICKS(500);
        vTaskDelay(delay);
    }
}

/* ── Print module API hooks ──────────────────────────────────────────────────
 *
 * Place print module init and sensor binding here.
 * The thermal print module (separate component) must:
 *
 *   1. Register its job callback:
 *        sony898_printer_iface_set_callback(thermal_on_job_ready);
 *
 *   2. Report sensor states from GPIO handlers / its own task:
 *        sony898_sensor_set_paper(gpio_get_level(PIN_PAPER_SENSOR));
 *        sony898_sensor_set_cover(gpio_get_level(PIN_COVER_SENSOR));
 *        sony898_sensor_set_ribbon(gpio_get_level(PIN_RIBBON_SENSOR));
 *        sony898_sensor_set_media_match(media_type_matches_job());
 *        sony898_sensor_set_system_error(hw_fault_detected());
 *
 *   3. Signal completion from its print task:
 *        sony898_printer_notify_done();   // success
 *        sony898_printer_notify_error();  // hardware fault
 *
 * ─────────────────────────────────────────────────────────────────────────────*/

static void printer_module_init(void) {
    /* TODO: replace with real thermal print module init
     *
     * Example:
     *   thermal_printer_init();
     *   sony898_printer_iface_set_callback(thermal_printer_on_job_ready);
     */
}

/* ── app_main ────────────────────────────────────────────────────────────────*/

void app_main(void) {
    ESP_LOGI(TAG, "Sony UP-D898MD_X898MD emulator starting");

    /* NVS — required for print counter */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* UART service interface */
    uart_config_t uart_cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, UART_BUF_SZ * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_cfg));

    /* Core modules */
    sony898_image_init();
    sony898_parser_init();
    sony898_status_init();
    sony898_sensor_init();
    sony898_counter_init();

    /* Thermal print module — registers callback and GPIO sensor bindings */
    printer_module_init();

    /* USB device */
    ESP_ERROR_CHECK(sony898_usb_init());
    sony898_usb_start_task();

    /* Tasks */
    xTaskCreate(uart_task,       "uart",   4096, NULL, 5, NULL);
    xTaskCreate(status_log_task, "status", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "ready — UART: status | device_id | usb | counter | "
             "dump_pgm | clear | info | set <state>");
}
