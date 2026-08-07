#pragma once

#include <stddef.h>
#include "esp_err.h"

esp_err_t tool_lua_init(void);
esp_err_t tool_lua_check_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_lua_run_execute(const char *input_json, char *output, size_t output_size);
