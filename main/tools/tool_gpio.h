#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize GPIO tool — configure allowed pins and directions.
 */
esp_err_t tool_gpio_init(void);

/**
 * Write a GPIO pin HIGH or LOW.
 * Input JSON: {"pin": <int>, "state": <0|1>}
 */
esp_err_t tool_gpio_write_execute(const char *input_json, char *output, size_t output_size);

/**
 * Read a single GPIO pin state.
 * Input JSON: {"pin": <int>}
 */
esp_err_t tool_gpio_read_execute(const char *input_json, char *output, size_t output_size);

/**
 * Read all allowed GPIO pin states at once.
 * Input JSON: {} (no parameters)
 */
esp_err_t tool_gpio_read_all_execute(const char *input_json, char *output, size_t output_size);
