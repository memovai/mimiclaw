#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute an HTTP request.
 *
 * @param input_json   JSON string with fields:
 *                     - "url" (required): HTTP or HTTPS URL
 *                     - "method" (optional): GET, POST, PUT, DELETE, PATCH, HEAD (default: GET)
 *                     - "headers" (optional): object of key-value header pairs
 *                     - "body" (optional): request body string
 * @param output       Output buffer for response text
 * @param output_size  Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t tool_http_request_execute(const char *input_json, char *output, size_t output_size);
