#include "sony898_image.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sony898_image";

static struct {
    uint8_t          *buf;
    size_t            size;
    uint16_t          width;
    uint16_t          height;
    volatile bool     ready;
    SemaphoreHandle_t lock;
} _img;

void sony898_image_init(void) {
    _img.lock = xSemaphoreCreateMutex();
    configASSERT(_img.lock);
}

esp_err_t sony898_image_alloc(uint16_t width, uint16_t height) {
    if (width == 0 || height == 0) return ESP_ERR_INVALID_ARG;

    size_t sz = (size_t)width * height;
    if (sz > MAX_IMAGE_BYTES) {
        ESP_LOGE(TAG, "image too large: %zu > %u", sz, MAX_IMAGE_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(_img.lock, portMAX_DELAY);

    if (_img.buf) {
        heap_caps_free(_img.buf);
        _img.buf = NULL;
    }

    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM free before alloc: %zu bytes", free_before);

    _img.buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!_img.buf) {
        xSemaphoreGive(_img.lock);
        ESP_LOGE(TAG, "PSRAM malloc failed for %zu bytes", sz);
        return ESP_ERR_NO_MEM;
    }

    /* Verify the allocation is actually in PSRAM. */
    if (!esp_ptr_external_ram(_img.buf)) {
        heap_caps_free(_img.buf);
        _img.buf = NULL;
        xSemaphoreGive(_img.lock);
        ESP_LOGE(TAG, "allocation is NOT in PSRAM — refusing");
        return ESP_ERR_NO_MEM;
    }

    _img.size   = sz;
    _img.width  = width;
    _img.height = height;
    _img.ready  = false;

    size_t free_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "allocated %zu bytes in PSRAM (free after: %zu)", sz, free_after);

    xSemaphoreGive(_img.lock);
    return ESP_OK;
}

uint8_t *sony898_image_get_write_ptr(void) {
    return _img.buf;
}

void sony898_image_mark_ready(void) {
    _img.ready = true;
}

bool sony898_image_ready(void) {
    return _img.ready;
}

const uint8_t *sony898_get_image_buffer(void) {
    return _img.ready ? _img.buf : NULL;
}

size_t sony898_get_image_size(void) {
    return _img.size;
}

uint16_t sony898_get_width(void) {
    return _img.width;
}

uint16_t sony898_get_height(void) {
    return _img.height;
}

void sony898_release_image(void) {
    xSemaphoreTake(_img.lock, portMAX_DELAY);
    if (_img.buf) {
        heap_caps_free(_img.buf);
        _img.buf    = NULL;
        _img.size   = 0;
        _img.width  = 0;
        _img.height = 0;
        _img.ready  = false;
        ESP_LOGI(TAG, "image buffer released");
    }
    xSemaphoreGive(_img.lock);
}
