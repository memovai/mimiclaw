#pragma once

#include "esp_err.h"
#include <stddef.h>

typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;  /* JSON Schema string for input */
    esp_err_t (*execute)(const char *input_json, char *output, size_t output_size);
} mimi_tool_t;

/**
 * Initialize tool registry and register all built-in tools.
 */
esp_err_t tool_registry_init(void);

/**
 * Get the pre-built tools JSON array string for the API request.
 * Returns NULL if no tools are registered.
 */
const char *tool_registry_get_tools_json(void);

/**
 * Execute a tool by name.
 *
 * @param name         Tool name (e.g. "web_search")
 * @param input_json   JSON string of tool input
 * @param output       Output buffer for tool result text
 * @param output_size  Size of output buffer
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if tool unknown
 */
esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size);

/**
 * Get access to the registered tools array.
 *
 * @param tools  Output: pointer to the tools array
 * @param count  Output: number of registered tools
 */
void tool_registry_get_tools(const mimi_tool_t **tools, int *count);
