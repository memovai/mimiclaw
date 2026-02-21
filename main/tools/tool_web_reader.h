#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize web reader tool (load Jina API key from NVS/build-time).
 */
esp_err_t tool_web_reader_init(void);

/**
 * Fetch a web page via Jina Reader API and return readable text.
 *
 * @param input_json   JSON string with "url" field
 * @param output       Output buffer for page content
 * @param output_size  Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t tool_web_reader_execute(const char *input_json, char *output, size_t output_size);

/**
 * Save Jina API key to NVS.
 */
esp_err_t tool_web_reader_set_key(const char *api_key);
