#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ESP-NOW message types */
#define ESPNOW_MSG_SCRIPT_START   0x01
#define ESPNOW_MSG_SCRIPT_CHUNK   0x02
#define ESPNOW_MSG_SCRIPT_END     0x03
#define ESPNOW_MSG_RESULT_START   0x11
#define ESPNOW_MSG_RESULT_CHUNK   0x12
#define ESPNOW_MSG_RESULT_END     0x13

/**
 * Initialize ESP-NOW and add the peer (Board B).
 * Must be called after WiFi is started.
 * Reads peer MAC from NVS; returns ESP_ERR_NOT_FOUND if not configured.
 */
esp_err_t espnow_manager_init(void);

/**
 * Send a Python script to Board B and wait for the execution result.
 *
 * @param script       Null-terminated Python source code
 * @param result       Buffer to receive execution output
 * @param result_size  Size of result buffer
 * @param timeout_ms   Max wait time in milliseconds
 * @return ESP_OK on success
 */
esp_err_t espnow_send_script(const char *script, char *result,
                              size_t result_size, int timeout_ms);

/**
 * Check if ESP-NOW is initialized and peer is configured.
 */
bool espnow_is_ready(void);
