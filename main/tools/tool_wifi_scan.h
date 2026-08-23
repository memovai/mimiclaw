#pragma once

#include "esp_err.h"

esp_err_t tool_wifi_scan_init(void);
esp_err_t tool_wifi_scan_execute(const char *input_json, char *output, size_t output_size);
