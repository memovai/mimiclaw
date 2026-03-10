#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute run_python tool.
 * Sends a Python script to Board B via ESP-NOW and returns the execution result.
 */
esp_err_t tool_run_python_execute(const char *input_json, char *output, size_t output_size);
