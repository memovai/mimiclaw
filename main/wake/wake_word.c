#include "wake_word.h"
#include "audio/audio_hal.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

static const char *TAG = "wake";

static wake_word_callback_t s_callback = NULL;
static const esp_wn_iface_t *s_wakenet = NULL;
static model_iface_data_t *s_wn_data = NULL;
static int s_audio_chunksize = 0;
static volatile bool s_running = false;
static TaskHandle_t s_task = NULL;

/* Single task: reads mic → runs WakeNet detect */
static void wake_task(void *arg)
{
    ESP_LOGI(TAG, "Wake word task started (chunksize=%d samples)", s_audio_chunksize);

    /* Allocate buffers */
    int16_t *mono_buf = malloc(s_audio_chunksize * sizeof(int16_t));
    int16_t *stereo_buf = malloc(s_audio_chunksize * 2 * sizeof(int16_t));
    if (!mono_buf || !stereo_buf) {
        ESP_LOGE(TAG, "Buffer allocation failed");
        free(mono_buf);
        free(stereo_buf);
        vTaskDelete(NULL);
        return;
    }

    esp_codec_dev_handle_t in_dev = audio_hal_get_input_dev();
    int detect_count = 0;

    while (s_running) {
        /* Read stereo from ES7210 via codec dev */
        int ret = esp_codec_dev_read(in_dev, stereo_buf,
                                     s_audio_chunksize * 2 * sizeof(int16_t));
        if (ret != 0) {
            ESP_LOGE(TAG, "Mic read error: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* Extract left channel */
        for (int i = 0; i < s_audio_chunksize; i++) {
            mono_buf[i] = stereo_buf[i * 2];
        }

        /* Run WakeNet detection */
        wakenet_state_t state = s_wakenet->detect(s_wn_data, mono_buf);

        if (state == WAKENET_DETECTED) {
            ESP_LOGW(TAG, "*** WAKE WORD DETECTED! ***");
            if (s_callback) {
                s_callback();
            }
        }

        /* Periodic heartbeat log */
        detect_count++;
        if (detect_count % 500 == 0) {
            ESP_LOGI(TAG, "Wake word listening... (%d chunks processed)", detect_count);
        }
    }

    free(mono_buf);
    free(stereo_buf);
    ESP_LOGI(TAG, "Wake word task stopped");
    vTaskDelete(NULL);
}

esp_err_t wake_word_init(wake_word_callback_t cb)
{
    s_callback = cb;

    ESP_LOGI(TAG, "Loading models from 'model' partition...");
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "Failed to load SR models");
        return ESP_FAIL;
    }

    /* Log all available models */
    ESP_LOGI(TAG, "Found %d model(s):", models->num);
    for (int i = 0; i < models->num; i++) {
        ESP_LOGI(TAG, "  [%d] %s", i, models->model_name[i]);
    }

    /* Find WakeNet model */
    char *wn_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    if (!wn_name) {
        ESP_LOGE(TAG, "No WakeNet model found! Check sdkconfig (CONFIG_SR_WN_WN9_*)");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Using WakeNet model: %s", wn_name);

    /* Get WakeNet interface */
    s_wakenet = esp_wn_handle_from_name(wn_name);
    if (!s_wakenet) {
        ESP_LOGE(TAG, "Failed to get WakeNet handle for: %s", wn_name);
        return ESP_FAIL;
    }

    /* Create WakeNet instance */
    s_wn_data = s_wakenet->create(wn_name, DET_MODE_90);
    if (!s_wn_data) {
        ESP_LOGE(TAG, "WakeNet create failed");
        return ESP_FAIL;
    }

    /* Get required audio chunk size */
    s_audio_chunksize = s_wakenet->get_samp_chunksize(s_wn_data);
    int sample_rate = s_wakenet->get_samp_rate(s_wn_data);
    ESP_LOGI(TAG, "WakeNet ready: chunksize=%d samples, sample_rate=%d Hz",
             s_audio_chunksize, sample_rate);

    return ESP_OK;
}

esp_err_t wake_word_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }
    if (!s_wakenet || !s_wn_data) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_running = true;
    xTaskCreatePinnedToCore(wake_task, "wake_word", 8 * 1024, NULL, 5, &s_task, 1);

    ESP_LOGI(TAG, "Wake word listening started");
    return ESP_OK;
}

esp_err_t wake_word_stop(void)
{
    if (!s_running) return ESP_OK;

    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(500));
    s_task = NULL;

    ESP_LOGI(TAG, "Wake word listening stopped");
    return ESP_OK;
}
