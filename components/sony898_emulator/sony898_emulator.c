#include "sony898_emulator.h"
#include "sony898_image.h"
#include "sony898_parser.h"
#include "sony898_status.h"
#include "sony898_sensors.h"
#include "sony898_printer_iface.h"
#include "sony898_counter.h"
#include "config.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG     = "sony898";
static const char *NVS_NS  = "sony898";
static const char *NVS_KEY = "serial";

#define SERIAL_MAX 32

static char _serial[SERIAL_MAX];

/* ── Serial helpers ──────────────────────────────────────────────────────── */

static void load_serial(const char *override) {
    if (override && override[0]) {
        strncpy(_serial, override, SERIAL_MAX - 1);
        _serial[SERIAL_MAX - 1] = '\0';
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = SERIAL_MAX;
        esp_err_t r = nvs_get_str(h, NVS_KEY, _serial, &len);
        nvs_close(h);
        if (r == ESP_OK && _serial[0]) return;
    }
    strncpy(_serial, CFG_USB_SERIAL, SERIAL_MAX - 1);
    _serial[SERIAL_MAX - 1] = '\0';
}

/* ── Internal status/dispatch task ──────────────────────────────────────── */

static void _status_task(void *arg) {
    (void)arg;
    bool       last_connected = false;
    bool       last_ready     = false;
    TickType_t printing_since = 0;
    TickType_t job_done_at    = 0;
    uint32_t   print_time_ms  = 0;
    bool       job_dispatched = false;

    while (1) {
        /* USB connect/disconnect monitoring */
        bool connected = sony898_usb_is_connected();
        if (connected != last_connected) {
            if (connected) {
                ESP_LOGI(TAG, "USB host connected — configured=%d ready=%d",
                         (int)sony898_usb_is_configured(),
                         (int)sony898_usb_is_ready_for_print());
            } else {
                ESP_LOGW(TAG, "USB host disconnected — state=%s",
                         sony898_status_state_name(sony898_status_get_state()));
            }
            last_connected = connected;
        }

        /* Image ready notification */
        bool ready = sony898_image_ready();
        if (ready && !last_ready) {
            ESP_LOGI(TAG, "image ready  %"PRIu16"x%"PRIu16"  %zu B  copies=%u",
                     sony898_get_width(), sony898_get_height(),
                     sony898_get_image_size(), sony898_parser_get_copies());
        }
        last_ready = ready;

        sony898_state_t state = sony898_status_get_state();

        if (state == SONY898_STATE_PRINTING) {

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
                    printing_since = xTaskGetTickCount();
                    uint16_t h = sony898_get_height();
                    if (h == 0) h = 960;
                    print_time_ms = ((uint32_t)h * 1000u) / CFG_PRINT_SPEED_LPS;
                    ESP_LOGI(TAG, "timer mode: %"PRIu16" lines → %"PRIu32" ms",
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
                       (xTaskGetTickCount() - printing_since) >=
                       pdMS_TO_TICKS(print_time_ms)) {
                sony898_status_set_state(SONY898_STATE_JOB_DONE);
                printing_since = 0;
                job_dispatched = false;
            }
            job_done_at = 0;

        } else if (state == SONY898_STATE_JOB_DONE) {
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

        TickType_t delay = (state == SONY898_STATE_PRINTING)
                           ? pdMS_TO_TICKS(50)
                           : pdMS_TO_TICKS(500);
        vTaskDelay(delay);
    }
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

esp_err_t sony898_emulator_init(const sony898_config_t *cfg) {
    load_serial(cfg ? cfg->serial : NULL);
    ESP_LOGI(TAG, "init — serial: %s", _serial);

    sony898_image_init();
    sony898_parser_init();
    sony898_status_init(_serial);
    sony898_sensor_init();
    sony898_counter_init();

    if (cfg) {
        if (cfg->on_job_ready)  sony898_printer_iface_set_callback(cfg->on_job_ready);
        if (cfg->on_connect)    sony898_usb_set_connect_cb(cfg->on_connect);
        if (cfg->on_disconnect) sony898_usb_set_disconnect_cb(cfg->on_disconnect);
    }

    return sony898_usb_init(
        cfg ? cfg->vid          : 0,
        cfg ? cfg->pid          : 0,
        cfg ? cfg->manufacturer : NULL,
        cfg ? cfg->product      : NULL,
        _serial
    );
}

void sony898_emulator_start(void) {
    xTaskCreate(_status_task, "sony898_st", 3072, NULL, 4, NULL);
    sony898_usb_start_task();
}

/* ── Identity ────────────────────────────────────────────────────────────── */

uint16_t    sony898_emulator_get_vid(void)          { return sony898_usb_get_vid(); }
uint16_t    sony898_emulator_get_pid(void)          { return sony898_usb_get_pid(); }
const char *sony898_emulator_get_manufacturer(void) { return sony898_usb_get_manufacturer(); }
const char *sony898_emulator_get_product(void)      { return sony898_usb_get_product(); }
const char *sony898_emulator_get_protocol(void)     { return CFG_DEV_CMD; }
const char *sony898_emulator_get_usb_class(void)    { return "7/1/2 Printer Bidirectional"; }

const char *sony898_emulator_get_serial(void) {
    return _serial;
}

esp_err_t sony898_emulator_set_serial(const char *serial) {
    if (!serial || !serial[0]) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = nvs_set_str(h, NVS_KEY, serial);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    if (ret == ESP_OK)
        ESP_LOGI(TAG, "serial saved to NVS: %s — reboot to apply", serial);
    else
        ESP_LOGE(TAG, "nvs write: %s", esp_err_to_name(ret));
    return ret;
}

const char *sony898_emulator_get_ieee1284_id(void) {
    return sony898_status_get_ieee1284_id();
}

/* ── USB state ───────────────────────────────────────────────────────────── */

bool sony898_emulator_usb_is_connected(void)  { return sony898_usb_is_connected(); }
bool sony898_emulator_usb_is_configured(void) { return sony898_usb_is_configured(); }
bool sony898_emulator_usb_is_ready(void)      { return sony898_usb_is_ready_for_print(); }

/* ── Printer state ───────────────────────────────────────────────────────── */

sony898_state_t sony898_emulator_get_state(void)      { return sony898_status_get_state(); }
void sony898_emulator_set_state(sony898_state_t s)    { sony898_status_set_state(s); }
const char *sony898_emulator_get_state_name(void)     { return sony898_status_state_name(sony898_status_get_state()); }
uint8_t sony898_emulator_get_port_status(void)        { return sony898_status_get_port_status(); }
bool sony898_emulator_is_printer_ready(void)          { return sony898_status_is_ready(); }

/* ── Image / job ─────────────────────────────────────────────────────────── */

bool           sony898_emulator_image_ready(void)       { return sony898_image_ready(); }
uint16_t       sony898_emulator_get_width(void)         { return sony898_get_width(); }
uint16_t       sony898_emulator_get_height(void)        { return sony898_get_height(); }
size_t         sony898_emulator_get_image_size(void)    { return sony898_get_image_size(); }
const uint8_t *sony898_emulator_get_image_buffer(void)  { return sony898_get_image_buffer(); }
uint8_t        sony898_emulator_get_copies(void)        { return sony898_parser_get_copies(); }
void           sony898_emulator_clear_job(void)         { sony898_parser_reset(); }

/* ── Sensor inputs ───────────────────────────────────────────────────────── */

void sony898_emulator_set_paper(bool v)        { sony898_sensor_set_paper(v); }
void sony898_emulator_set_cover(bool v)        { sony898_sensor_set_cover(v); }
void sony898_emulator_set_ribbon(bool v)       { sony898_sensor_set_ribbon(v); }
void sony898_emulator_set_media_match(bool v)  { sony898_sensor_set_media_match(v); }
void sony898_emulator_set_system_error(bool v) { sony898_sensor_set_system_error(v); }
void sony898_emulator_clear_sensors(void)      { sony898_sensor_clear_all(); }
bool sony898_emulator_has_sensor_error(void)   { return sony898_sensor_has_error(); }

/* ── Print completion ────────────────────────────────────────────────────── */

void sony898_emulator_notify_done(void)        { sony898_printer_notify_done(); }
void sony898_emulator_notify_error(void)       { sony898_printer_notify_error(); }
bool sony898_emulator_has_print_module(void)   { return sony898_printer_iface_has_module(); }

/* ── Statistics ──────────────────────────────────────────────────────────── */

uint32_t sony898_emulator_get_print_count(void) { return sony898_counter_get(); }
