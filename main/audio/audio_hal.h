#pragma once

#include "esp_err.h"
#include "esp_codec_dev.h"
#include "driver/i2s_std.h"

/**
 * Initialize audio hardware:
 *  - I2C bus for codec control
 *  - I2S channel pair (TX for speaker, RX for mic)
 *  - ES8311 (speaker DAC) and ES7210 (mic ADC) via esp_codec_dev
 *  - Speaker PA GPIO (starts disabled)
 *
 * Call once from app_main() before any audio operations.
 */
esp_err_t audio_hal_init(void);

/**
 * Get the esp_codec_dev output device (ES8311 speaker).
 * Use esp_codec_dev_write() to play audio.
 * Only valid after audio_hal_init() succeeds.
 */
esp_codec_dev_handle_t audio_hal_get_output_dev(void);

/**
 * Get the esp_codec_dev input device (ES7210 mic).
 * Use esp_codec_dev_read() to record audio.
 * Only valid after audio_hal_init() succeeds.
 */
esp_codec_dev_handle_t audio_hal_get_input_dev(void);

/**
 * Get the I2S TX channel handle (for speaker output).
 * Only valid after audio_hal_init() succeeds.
 */
i2s_chan_handle_t audio_hal_get_tx(void);

/**
 * Get the I2S RX channel handle (for mic input).
 * Only valid after audio_hal_init() succeeds.
 */
i2s_chan_handle_t audio_hal_get_rx(void);

/**
 * Enable or disable the speaker power amplifier.
 */
void audio_hal_speaker_pa(bool enable);

/**
 * Set speaker output volume (0-100).
 */
esp_err_t audio_hal_set_volume(int volume);
