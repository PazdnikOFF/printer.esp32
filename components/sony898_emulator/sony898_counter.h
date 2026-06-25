#pragma once
#include <stdint.h>
#include "esp_err.h"

/*
 * Persistent print counter stored in NVS.
 * Survives power cycles.
 *
 * Call sony898_counter_init() once before use (after nvs_flash_init()).
 */

esp_err_t sony898_counter_init(void);

/* Total number of successfully completed print jobs. */
uint32_t  sony898_counter_get(void);

/* Increment by 1 and persist to NVS.  Call after JOB_DONE. */
esp_err_t sony898_counter_increment(void);
