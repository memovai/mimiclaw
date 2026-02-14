#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * Play a sine wave test tone through the speaker.
 * Blocks until playback completes.
 *
 * @param freq_hz   Tone frequency (e.g. 440 for A4)
 * @param duration_ms  Duration in milliseconds
 */
esp_err_t audio_player_play_tone(uint32_t freq_hz, uint32_t duration_ms);

/**
 * Play raw PCM audio data through the speaker.
 * Data must be 16-bit signed, mono, at MIMI_AUDIO_SAMPLE_RATE.
 * Internally duplicates to stereo for the I2S bus.
 * Blocks until playback completes.
 *
 * @param pcm_data  16-bit signed PCM samples (mono)
 * @param pcm_len   Length in bytes
 */
esp_err_t audio_player_play_pcm(const int16_t *pcm_data, size_t pcm_len);

/**
 * Play a WAV buffer (with 44-byte header) through the speaker.
 * Parses the header, extracts PCM data, and plays it.
 */
esp_err_t audio_player_play_wav(const uint8_t *wav_data, size_t wav_len);
