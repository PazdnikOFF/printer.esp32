#include "sony898_counter.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG       = "sony898_counter";
static const char *NVS_NS    = "sony898";
static const char *NVS_KEY   = "print_cnt";

static uint32_t _count = 0;

esp_err_t sony898_counter_init(void) {
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        _count = 0;
        ESP_LOGI(TAG, "counter init: 0 (new)");
        return ESP_OK;
    }
    if (ret != ESP_OK) return ret;

    ret = nvs_get_u32(h, NVS_KEY, &_count);
    nvs_close(h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        _count = 0;
        ESP_LOGI(TAG, "counter init: 0 (key not found)");
        return ESP_OK;
    }
    if (ret != ESP_OK) return ret;
    ESP_LOGI(TAG, "counter init: %"PRIu32, _count);
    return ESP_OK;
}

uint32_t sony898_counter_get(void) {
    return _count;
}

esp_err_t sony898_counter_increment(void) {
    _count++;
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = nvs_set_u32(h, NVS_KEY, _count);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs write failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "print count: %"PRIu32, _count);
    return ESP_OK;
}
