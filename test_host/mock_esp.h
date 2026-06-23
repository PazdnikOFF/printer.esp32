/*
 * Minimal ESP-IDF mocks for host-side unit testing of sony898_parser.
 *
 * Build:
 *   gcc -std=c11 -Wall -I test_host -I components/sony898_emulator \
 *       components/sony898_emulator/sony898_image.c  \
 *       components/sony898_emulator/sony898_status.c \
 *       components/sony898_emulator/sony898_parser.c \
 *       test_host/test_parser.c                      \
 *       -o test_host/test_parser
 */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <inttypes.h>
#include <stdatomic.h>

/* esp_err_t */
typedef int esp_err_t;
#define ESP_OK                0
#define ESP_FAIL             -1
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_INVALID_SIZE  0x103
#define ESP_ERR_NO_MEM        0x101

/* Logging */
#define ESP_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) printf("[D][%s] " fmt "\n", tag, ##__VA_ARGS__)

/* PSRAM: just malloc on host */
#define MALLOC_CAP_SPIRAM  0u
static inline void *heap_caps_malloc(size_t sz, uint32_t caps) {
    (void)caps; return malloc(sz);
}
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(uint32_t caps) {
    (void)caps; return 8u * 1024u * 1024u;
}
static inline bool esp_ptr_external_ram(const void *p) {
    (void)p; return true; /* always "PSRAM" on host */
}

/* FreeRTOS stubs */
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xFFFFFFFFu
#define xSemaphoreCreateMutex() ((void *)1)
#define xSemaphoreTake(s, t)    ((void)0)
#define xSemaphoreGive(s)       ((void)0)
#define configASSERT(x)         do { if (!(x)) { fprintf(stderr, "ASSERT\n"); exit(1); } } while(0)
