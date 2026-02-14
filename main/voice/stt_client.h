#pragma once

#include "esp_err.h"

/**
 * Transcribe PCM audio via faster-whisper HTTP API.
 *
 * @param pcm_data   Mono 16-bit 16kHz PCM samples
 * @param pcm_len    Length in bytes
 * @param text_out   Output buffer for transcribed text
 * @param text_size  Size of text_out buffer
 * @return ESP_OK on success
 */
esp_err_t stt_transcribe(const int16_t *pcm_data, size_t pcm_len,
                         char *text_out, size_t text_size);
