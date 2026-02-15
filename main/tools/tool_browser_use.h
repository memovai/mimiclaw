#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t tool_browser_use_execute(const char *input_json, char *output, size_t output_size);
