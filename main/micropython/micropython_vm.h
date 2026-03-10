#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * One-time initialization: creates mutex and timeout timer.
 * Call after SPIFFS init, before any exec calls.
 */
esp_err_t micropython_vm_init(void);

/**
 * Execute Python code string and capture stdout output.
 *
 * Thread-safe (mutex-protected, one script at a time).
 * Allocates GC heap from PSRAM per call, frees on return.
 *
 * @param code        Python source code to execute
 * @param output      Buffer to receive captured stdout (and exception text)
 * @param output_size Size of output buffer
 * @param timeout_ms  Max execution time (0 = no timeout)
 * @return ESP_OK on success, error code on failure
 */
esp_err_t micropython_vm_exec(const char *code, char *output, size_t output_size,
                               int timeout_ms);
