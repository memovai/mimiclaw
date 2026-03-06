#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Initialize web search tool.
 */
esp_err_t tool_web_search_init(void);

/**
 * Execute a web search.
 *
 * @param input_json   JSON string with "query" field
 * @param output       Output buffer for formatted search results
 * @param output_size  Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t tool_web_search_execute(const char *input_json, char *output, size_t output_size);

/**
 * Save Brave Search API key to NVS.
 */
esp_err_t tool_web_search_set_key(const char *api_key);

/**
 * Save Tavily API key to NVS.
 */
esp_err_t tool_web_search_set_tavily_key(const char *api_key);
