#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* Maximum image size accepted from host (fits in 8 MB PSRAM with margin). */
#define MAX_IMAGE_BYTES (6u * 1024u * 1024u)

void        sony898_image_init(void);
esp_err_t   sony898_image_alloc(uint16_t width, uint16_t height);
uint8_t    *sony898_image_get_write_ptr(void);
void        sony898_image_mark_ready(void);
void        sony898_release_image(void);

bool            sony898_image_ready(void);
const uint8_t  *sony898_get_image_buffer(void);
size_t          sony898_get_image_size(void);
uint16_t        sony898_get_width(void);
uint16_t        sony898_get_height(void);
