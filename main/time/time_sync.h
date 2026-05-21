#pragma once

#include "esp_err.h"
#include <stdbool.h>

/**
 * Start SNTP synchronization in the background.
 *
 * Configures the system timezone from MIMI_TIMEZONE and asks lwIP/SNTP
 * to keep the RTC in sync with pool.ntp.org. Returns immediately; the
 * first sync arrives a few seconds later via a background callback.
 * Safe to call multiple times — only the first call has effect.
 * Should be called after WiFi has an IP.
 */
esp_err_t time_sync_start(void);

/**
 * Block until the system clock has been set at least once, or timeout.
 * @param timeout_ms  Max wait (0 returns immediately, portMAX_DELAY = forever)
 * @return true if time is set, false on timeout
 */
bool time_sync_wait(uint32_t timeout_ms);

/**
 * Whether the system clock has been set at least once since boot.
 */
bool time_sync_is_set(void);
