#include "audio_hal.h"
#include "mimi_config.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

static const char *TAG = "audio_test";

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void audio_test_run(void)
{
    ESP_LOGW(TAG, "===== AUDIO DEBUG TEST START =====");

    /* --- Check handles --- */
    i2s_chan_handle_t tx = audio_hal_get_tx();
    i2s_chan_handle_t rx = audio_hal_get_rx();
    esp_codec_dev_handle_t out_dev = audio_hal_get_output_dev();
    esp_codec_dev_handle_t in_dev = audio_hal_get_input_dev();

    ESP_LOGI(TAG, "I2S TX handle: %p", tx);
    ESP_LOGI(TAG, "I2S RX handle: %p", rx);
    ESP_LOGI(TAG, "Output dev: %p", out_dev);
    ESP_LOGI(TAG, "Input dev: %p", in_dev);

    if (!tx) {
        ESP_LOGE(TAG, "NO TX HANDLE - cannot test speaker");
        return;
    }

    /* --- Generate 1 second of 1kHz tone (stereo, 16-bit) --- */
    const int sample_rate = MIMI_AUDIO_SAMPLE_RATE;
    const int duration_ms = 1000;
    const int total_samples = sample_rate * duration_ms / 1000;
    const int stereo_bytes = total_samples * 2 * sizeof(int16_t);
    int16_t *buf = malloc(stereo_bytes);
    if (!buf) {
        ESP_LOGE(TAG, "malloc failed for %d bytes", stereo_bytes);
        return;
    }

    for (int i = 0; i < total_samples; i++) {
        float t = (float)i / (float)sample_rate;
        int16_t sample = (int16_t)(10000.0f * sinf(2.0f * M_PI * 1000.0f * t));
        buf[i * 2]     = sample;  /* L */
        buf[i * 2 + 1] = sample;  /* R */
    }
    ESP_LOGI(TAG, "Generated 1kHz tone: %d samples, %d bytes stereo", total_samples, stereo_bytes);

    /* ============================================================
     * TEST 1: Raw I2S write (bypass esp_codec_dev completely)
     * ============================================================ */
    ESP_LOGW(TAG, "----- TEST 1: Raw I2S write -----");

    /* Force PA on */
    gpio_set_level(MIMI_SPEAKER_EN, 1);
    ESP_LOGI(TAG, "PA GPIO %d set HIGH", MIMI_SPEAKER_EN);

    /* Try to enable TX channel (may already be enabled by esp_codec_dev_open) */
    esp_err_t ret = i2s_channel_enable(tx);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "i2s_channel_enable(tx) OK");
    } else {
        ESP_LOGW(TAG, "i2s_channel_enable(tx) returned: %s (may already be enabled)", esp_err_to_name(ret));
    }

    /* Write in chunks */
    size_t offset = 0;
    int write_count = 0;
    while (offset < (size_t)stereo_bytes) {
        size_t to_write = stereo_bytes - offset;
        if (to_write > 2048) to_write = 2048;

        size_t written = 0;
        ret = i2s_channel_write(tx, (uint8_t *)buf + offset, to_write, &written, pdMS_TO_TICKS(1000));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_write failed at offset %d: %s", (int)offset, esp_err_to_name(ret));
            break;
        }
        offset += written;
        write_count++;
    }
    ESP_LOGI(TAG, "Raw I2S: wrote %d bytes in %d chunks", (int)offset, write_count);

    /* Wait a bit for DMA to flush */
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(MIMI_SPEAKER_EN, 0);
    ESP_LOGI(TAG, "PA off");

    vTaskDelay(pdMS_TO_TICKS(500));

    /* ============================================================
     * TEST 2: esp_codec_dev_write
     * ============================================================ */
    ESP_LOGW(TAG, "----- TEST 2: esp_codec_dev_write -----");

    if (!out_dev) {
        ESP_LOGE(TAG, "Output device is NULL, skipping test 2");
    } else {
        gpio_set_level(MIMI_SPEAKER_EN, 1);
        ESP_LOGI(TAG, "PA on");

        offset = 0;
        write_count = 0;
        while (offset < (size_t)stereo_bytes) {
            size_t to_write = stereo_bytes - offset;
            if (to_write > 2048) to_write = 2048;

            int cret = esp_codec_dev_write(out_dev, (uint8_t *)buf + offset, (int)to_write);
            if (cret != 0) {
                ESP_LOGE(TAG, "esp_codec_dev_write failed at offset %d: %d", (int)offset, cret);
                break;
            }
            offset += to_write;
            write_count++;
        }
        ESP_LOGI(TAG, "codec_dev: wrote %d bytes in %d chunks", (int)offset, write_count);

        vTaskDelay(pdMS_TO_TICKS(200));
        gpio_set_level(MIMI_SPEAKER_EN, 0);
        ESP_LOGI(TAG, "PA off");
    }

    vTaskDelay(pdMS_TO_TICKS(500));

    /* ============================================================
     * TEST 3: Mic loopback (read mic → write speaker, 5 seconds)
     * ============================================================ */
    ESP_LOGW(TAG, "----- TEST 3: Mic loopback 5s -----");

    if (!in_dev || !out_dev) {
        ESP_LOGE(TAG, "Input or output dev NULL (in=%p, out=%p), skipping loopback", in_dev, out_dev);
    } else {
        gpio_set_level(MIMI_SPEAKER_EN, 1);
        ESP_LOGI(TAG, "PA on — speak into mic now!");

        const int read_bytes = 1024;
        uint8_t *mic_buf = malloc(read_bytes);
        if (!mic_buf) {
            ESP_LOGE(TAG, "malloc failed for mic buf");
        } else {
            int loops = (sample_rate * 2 * sizeof(int16_t) * 5) / read_bytes;  /* ~5 seconds */
            int loud_count = 0;

            for (int i = 0; i < loops; i++) {
                int rret = esp_codec_dev_read(in_dev, mic_buf, read_bytes);
                if (rret != 0) {
                    ESP_LOGE(TAG, "esp_codec_dev_read failed: %d (loop %d)", rret, i);
                    break;
                }

                /* Compute RMS of read data */
                int16_t *samples = (int16_t *)mic_buf;
                int num_samples = read_bytes / sizeof(int16_t);
                int64_t sum_sq = 0;
                for (int s = 0; s < num_samples; s++) {
                    sum_sq += (int64_t)samples[s] * samples[s];
                }
                int rms = (int)sqrtf((float)sum_sq / num_samples);

                if (i % 50 == 0) {
                    ESP_LOGI(TAG, "Mic RMS: %d (loop %d/%d)%s", rms, i, loops, rms > 500 ? " <<< SOUND" : "");
                }
                if (rms > 500) loud_count++;

                /* Write to speaker */
                int wret = esp_codec_dev_write(out_dev, mic_buf, read_bytes);
                if (wret != 0) {
                    ESP_LOGE(TAG, "esp_codec_dev_write failed in loopback: %d", wret);
                    break;
                }
            }

            ESP_LOGI(TAG, "Loopback done. Loud frames: %d/%d", loud_count, loops);
            free(mic_buf);
        }

        gpio_set_level(MIMI_SPEAKER_EN, 0);
        ESP_LOGI(TAG, "PA off");
    }

    free(buf);
    ESP_LOGW(TAG, "===== AUDIO DEBUG TEST DONE =====");
}
