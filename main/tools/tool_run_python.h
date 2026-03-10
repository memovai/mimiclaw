#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Execute run_python tool.
 * Runs Python code in the embedded MicroPython VM and returns captured stdout.
 */
esp_err_t tool_run_python_execute(const char *input_json, char *output, size_t output_size);
