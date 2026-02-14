#include "audio_player.h"
#include "audio_hal.h"
#include "mimi_config.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"

static const char *TAG = "audio_player";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Write mono PCM via esp_codec_dev (stereo output).
 * Duplicates each mono sample to L+R channels. */
static esp_err_t write_stereo(esp_codec_dev_handle_t dev, const int16_t *mono, size_t mono_samples)
{
    const size_t CHUNK = 512;  /* mono samples per chunk */
    int16_t stereo_buf[CHUNK * 2];

    size_t written_samples = 0;
    while (written_samples < mono_samples) {
        size_t chunk = mono_samples - written_samples;
        if (chunk > CHUNK) chunk = CHUNK;

        /* Interleave mono → stereo (L=R) */
        for (size_t i = 0; i < chunk; i++) {
            stereo_buf[i * 2]     = mono[written_samples + i];  /* L */
            stereo_buf[i * 2 + 1] = mono[written_samples + i];  /* R */
        }

        int ret = esp_codec_dev_write(dev, stereo_buf, chunk * 2 * sizeof(int16_t));
        if (ret != 0) return ESP_FAIL;

        written_samples += chunk;
    }
    return ESP_OK;
}

esp_err_t audio_player_play_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    esp_codec_dev_handle_t dev = audio_hal_get_output_dev();
    if (!dev) {
        ESP_LOGE(TAG, "Output device not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Playing tone: %lu Hz, %lu ms", (unsigned long)freq_hz, (unsigned long)duration_ms);

    uint32_t sample_rate = MIMI_AUDIO_SAMPLE_RATE;
    uint32_t total_samples = (sample_rate * duration_ms) / 1000;

    /* Generate mono sine wave in chunks */
    const size_t GEN_CHUNK = 1024;
    int16_t *mono_buf = malloc(GEN_CHUNK * sizeof(int16_t));
    if (!mono_buf) return ESP_ERR_NO_MEM;

    /* Enable speaker PA */
    audio_hal_speaker_pa(true);

    esp_err_t ret = ESP_OK;
    uint32_t generated = 0;
    while (generated < total_samples) {
        uint32_t chunk = total_samples - generated;
        if (chunk > GEN_CHUNK) chunk = GEN_CHUNK;

        for (uint32_t i = 0; i < chunk; i++) {
            float t = (float)(generated + i) / (float)sample_rate;
            mono_buf[i] = (int16_t)(10000.0f * sinf(2.0f * (float)M_PI * freq_hz * t));
        }

        ret = write_stereo(dev, mono_buf, chunk);
        if (ret != ESP_OK) break;

        generated += chunk;
    }

    audio_hal_speaker_pa(false);
    free(mono_buf);

    ESP_LOGI(TAG, "Tone playback complete");
    return ret;
}

esp_err_t audio_player_play_pcm(const int16_t *pcm_data, size_t pcm_len)
{
    esp_codec_dev_handle_t dev = audio_hal_get_output_dev();
    if (!dev) return ESP_ERR_INVALID_STATE;
    if (!pcm_data || pcm_len == 0) return ESP_ERR_INVALID_ARG;

    size_t total_samples = pcm_len / sizeof(int16_t);
    ESP_LOGI(TAG, "Playing PCM: %d samples (%.1f seconds)",
             (int)total_samples, (float)total_samples / MIMI_AUDIO_SAMPLE_RATE);

    audio_hal_speaker_pa(true);
    esp_err_t ret = write_stereo(dev, pcm_data, total_samples);
    audio_hal_speaker_pa(false);

    ESP_LOGI(TAG, "PCM playback complete");
    return ret;
}

esp_err_t audio_player_play_wav(const uint8_t *wav_data, size_t wav_len)
{
    if (!wav_data || wav_len < 44) return ESP_ERR_INVALID_ARG;

    /* Parse WAV header (minimal validation) */
    if (memcmp(wav_data, "RIFF", 4) != 0 || memcmp(wav_data + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Invalid WAV header");
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t channels = *(uint16_t *)(wav_data + 22);
    uint32_t sample_rate = *(uint32_t *)(wav_data + 24);
    uint16_t bits = *(uint16_t *)(wav_data + 34);

    ESP_LOGI(TAG, "WAV: %d ch, %lu Hz, %d bit", channels, (unsigned long)sample_rate, bits);

    if (bits != 16) {
        ESP_LOGE(TAG, "Only 16-bit WAV supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Find data chunk */
    size_t pos = 12;
    while (pos + 8 < wav_len) {
        uint32_t chunk_size = *(uint32_t *)(wav_data + pos + 4);
        if (memcmp(wav_data + pos, "data", 4) == 0) {
            const int16_t *pcm = (const int16_t *)(wav_data + pos + 8);
            size_t pcm_bytes = chunk_size;
            if (pos + 8 + pcm_bytes > wav_len) pcm_bytes = wav_len - pos - 8;

            if (channels == 1) {
                return audio_player_play_pcm(pcm, pcm_bytes);
            } else {
                /* Stereo → mono: take left channel */
                size_t stereo_samples = pcm_bytes / (2 * sizeof(int16_t));
                int16_t *mono = heap_caps_malloc(stereo_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
                if (!mono) return ESP_ERR_NO_MEM;

                for (size_t i = 0; i < stereo_samples; i++) {
                    mono[i] = pcm[i * 2];
                }
                esp_err_t ret = audio_player_play_pcm(mono, stereo_samples * sizeof(int16_t));
                free(mono);
                return ret;
            }
        }
        pos += 8 + chunk_size;
    }

    ESP_LOGE(TAG, "WAV data chunk not found");
    return ESP_ERR_INVALID_ARG;
}
