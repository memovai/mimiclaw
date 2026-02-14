#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

/**
 * Initialize web search tool.
 */
esp_err_t tool_web_search_init(void);

/**
 * Check if web search is available (Brave or Volcengine).
 */
bool tool_web_search_is_available(void);

/**
 * Get the current search provider name.
 */
const char *tool_web_search_get_provider(void);

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
 * Save Volcengine API key to NVS.
 */
esp_err_t tool_web_search_set_volcengine_key(const char *api_key);

/**
 * Save Volcengine model to NVS.
 */
esp_err_t tool_web_search_set_volcengine_model(const char *model);
