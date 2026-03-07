#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute get_current_time tool.
 * Fetches current time via HTTP Date header, sets system clock, returns time string.
 */
esp_err_t tool_get_time_execute(const char *input_json, char *output, size_t output_size);

/** Load timezone from NVS / build-time secret / hardcoded default and apply it. */
void timezone_init(void);

/** Write a new timezone to NVS and apply it immediately. Returns ESP_ERR_INVALID_ARG if empty, ESP_ERR_INVALID_SIZE if too long. */
esp_err_t timezone_set(const char *tz);

/** Return the current effective POSIX TZ string. */
const char *timezone_get(void);
