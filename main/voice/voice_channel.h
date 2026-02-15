#pragma once

#include "esp_err.h"

/**
 * Initialize the voice channel.
 * Sets up button callbacks, allocates PSRAM buffers, and starts the voice task.
 * Call after WiFi and network-dependent services are ready.
 */
esp_err_t voice_channel_init(void);

/**
 * Send a text response to the voice channel (play via TTS + display).
 * Called by outbound dispatch task.
 *
 * @param text  Text response from LLM
 */
void voice_channel_send_response(const char *text);

/**
 * Get current voice channel state (for debugging/CLI).
 */
const char *voice_channel_get_state(void);
