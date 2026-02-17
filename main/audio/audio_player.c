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

typedef struct {
    float b0, b1, b2;
    float a1, a2;
    float z1, z2;
} biquad_filter_t;

static void biquad_init_highpass(biquad_filter_t *f, float fc, float fs) {
    float w0 = 2.0f * M_PI * fc / fs;
    float cos_w0 = cosf(w0);
    float sin_w0 = sinf(w0);
    float alpha = sin_w0 / (2.0f * 0.707f);
    
    float a0 = 1.0f + alpha;
    f->b0 = (1.0f + cos_w0) / (2.0f * a0);
    f->b1 = -(1.0f + cos_w0) / a0;
    f->b2 = (1.0f + cos_w0) / (2.0f * a0);
    f->a1 = (-2.0f * cos_w0) / a0;
    f->a2 = (1.0f - alpha) / a0;
    f->z1 = f->z2 = 0.0f;
}

static void biquad_init_lowpass(biquad_filter_t *f, float fc, float fs) {
    float w0 = 2.0f * M_PI * fc / fs;
    float cos_w0 = cosf(w0);
    float sin_w0 = sinf(w0);
    float alpha = sin_w0 / (2.0f * 0.707f);
    
    float a0 = 1.0f + alpha;
    f->b0 = (1.0f - cos_w0) / (2.0f * a0);
    f->b1 = (1.0f - cos_w0) / a0;
    f->b2 = (1.0f - cos_w0) / (2.0f * a0);
    f->a1 = (-2.0f * cos_w0) / a0;
    f->a2 = (1.0f - alpha) / a0;
    f->z1 = f->z2 = 0.0f;
}

static float biquad_process(biquad_filter_t *f, float in) {
    float out = f->b0 * in + f->z1;
    f->z1 = f->b1 * in - f->a1 * out + f->z2;
    f->z2 = f->b2 * in - f->a2 * out;
    return out;
}

static void apply_audio_cleanup(int16_t *samples, size_t num_samples, uint32_t sample_rate) {
    if (num_samples < 100) return;
    
    biquad_filter_t hp_filter, lp_filter;
    biquad_init_highpass(&hp_filter, 80.0f, (float)sample_rate);
    biquad_init_lowpass(&lp_filter, 7000.0f, (float)sample_rate);
    
    for (size_t i = 0; i < num_samples; i++) {
        float sample = (float)samples[i];
        sample = biquad_process(&hp_filter, sample);
        sample = biquad_process(&lp_filter, sample);
        
        if (sample > 32767.0f) sample = 32767.0f;
        if (sample < -32768.0f) sample = -32768.0f;
        
        samples[i] = (int16_t)sample;
    }
    
    size_t fade_samples = (size_t)(0.05f * sample_rate);
    if (fade_samples > num_samples / 2) fade_samples = num_samples / 2;
    
    for (size_t i = 0; i < fade_samples; i++) {
        size_t idx = num_samples - fade_samples + i;
        float fade = 1.0f - ((float)i / (float)fade_samples);
        samples[idx] = (int16_t)((float)samples[idx] * fade);
    }
}

/* DMA drain time in ms: one full DMA buffer depth + margin */
#define DMA_DRAIN_MS  ((MIMI_I2S_DMA_DESC_NUM * MIMI_I2S_DMA_FRAME_NUM * 1000) / MIMI_AUDIO_SAMPLE_RATE + 50)

/* Write mono PCM via esp_codec_dev (stereo output).
 * Duplicates each mono sample to L+R channels. */
static esp_err_t write_stereo(esp_codec_dev_handle_t dev, const int16_t *mono, size_t mono_samples)
{
    const size_t CHUNK = 512;
    int16_t stereo_buf[CHUNK * 2];

    size_t written_samples = 0;
    while (written_samples < mono_samples) {
        size_t chunk = mono_samples - written_samples;
        if (chunk > CHUNK) chunk = CHUNK;

        for (size_t i = 0; i < chunk; i++) {
            stereo_buf[i * 2]     = mono[written_samples + i];
            stereo_buf[i * 2 + 1] = mono[written_samples + i];
        }

        int ret = esp_codec_dev_write(dev, stereo_buf, chunk * 2 * sizeof(int16_t));
        if (ret != 0) return ESP_FAIL;

        written_samples += chunk;
    }
    return ESP_OK;
}

/* Flush DMA with silence so the last audio frames are pushed out cleanly.
 * Writes one full DMA buffer depth of zeros in stereo. */
static void flush_dma_silence(esp_codec_dev_handle_t dev)
{
    const size_t FLUSH_FRAMES = MIMI_I2S_DMA_DESC_NUM * MIMI_I2S_DMA_FRAME_NUM;
    const size_t CHUNK = 256;
    int16_t silence[CHUNK * 2];
    memset(silence, 0, sizeof(silence));

    size_t flushed = 0;
    while (flushed < FLUSH_FRAMES) {
        size_t chunk = FLUSH_FRAMES - flushed;
        if (chunk > CHUNK) chunk = CHUNK;
        esp_codec_dev_write(dev, silence, chunk * 2 * sizeof(int16_t));
        flushed += chunk;
    }
}

/* Clean shutdown: silence flush → hardware mute → wait DMA drain → disable PA → unmute */
static void audio_shutdown(esp_codec_dev_handle_t dev)
{
    flush_dma_silence(dev);
    esp_codec_dev_set_out_mute(dev, true);
    vTaskDelay(pdMS_TO_TICKS(DMA_DRAIN_MS));
    audio_hal_speaker_pa(false);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_codec_dev_set_out_mute(dev, false);
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

    free(mono_buf);
    audio_shutdown(dev);
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

    int16_t *filtered_pcm = heap_caps_malloc(pcm_len, MALLOC_CAP_SPIRAM);
    if (!filtered_pcm) {
        ESP_LOGE(TAG, "Failed to allocate filter buffer");
        return ESP_ERR_NO_MEM;
    }
    
    memcpy(filtered_pcm, pcm_data, pcm_len);
    
    ESP_LOGI(TAG, "Applying audio cleanup filters...");
    apply_audio_cleanup(filtered_pcm, total_samples, MIMI_AUDIO_SAMPLE_RATE);

    audio_hal_speaker_pa(true);

    esp_err_t ret = write_stereo(dev, filtered_pcm, total_samples);
    free(filtered_pcm);

    audio_shutdown(dev);
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
