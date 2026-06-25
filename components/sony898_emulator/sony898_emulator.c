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
#include <string.h>

static const char *TAG     = "sony898";
static const char *NVS_NS  = "sony898";
static const char *NVS_KEY = "serial";

#define SERIAL_MAX 32

static char _serial[SERIAL_MAX];

/* ── serial resolution ───────────────────────────────────────────────────── */

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

/* ── public API ──────────────────────────────────────────────────────────── */

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

    return sony898_usb_init(_serial);
}

void sony898_emulator_start(void) {
    sony898_usb_start_task();
}

const char *sony898_emulator_get_serial(void) {
    return _serial;
}

esp_err_t sony898_emulator_set_serial(const char *serial) {
    if (!serial || !serial[0]) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = nvs_set_str(h, NVS_KEY, serial);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "serial written to NVS: %s — reboot to apply", serial);
    } else {
        ESP_LOGE(TAG, "nvs write failed: %s", esp_err_to_name(ret));
    }
    return ret;
}
