#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * Synthesize speech via TTS HTTP API.
 *
 * @param text        Text to synthesize
 * @param wav_out     Output buffer (caller-allocated, PSRAM recommended)
 * @param wav_out_cap Capacity of wav_out buffer in bytes
 * @param wav_len     Actual bytes written to wav_out
 * @return ESP_OK on success
 */
esp_err_t tts_synthesize(const char *text, uint8_t *wav_out, size_t wav_out_cap,
                         size_t *wav_len);
