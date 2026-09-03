#pragma once

#include "esp_err.h"
#include "esp_bit_defs.h"
#include <stdbool.h>

#define HEARTBEAT_SEND_BIT BIT0

/**
 * Initialize the heartbeat service (logs ready state).
 */
esp_err_t heartbeat_init(void);

/**
 * Start the heartbeat timer. Checks HEARTBEAT.md periodically
 * and sends a prompt to the agent if actionable tasks are found.
 */
esp_err_t heartbeat_start(void);

/**
 * Stop and delete the heartbeat timer.
 */
void heartbeat_stop(void);

/**
 * Manually trigger a heartbeat check (for CLI testing).
 * Returns true if the agent was prompted, false if no tasks found.
 */
bool heartbeat_trigger(void);
