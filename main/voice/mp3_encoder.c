#include "mp3_encoder.h"
#include "layer3.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "mp3_encoder";

static shine_t s_encoder = NULL;
static int s_samples_per_pass = 0;

esp_err_t mp3_encoder_init(void)
{
    if (s_encoder) {
        ESP_LOGW(TAG, "Encoder already initialized");
        return ESP_OK;
    }

    shine_config_t config;
    shine_set_config_mpeg_defaults(&config.mpeg);
    
    config.mpeg.mode = MONO;
    config.mpeg.bitr = MIMI_MP3_BITRATE;
    config.wave.channels = PCM_MONO;
    config.wave.samplerate = MIMI_AUDIO_SAMPLE_RATE;

    ESP_LOGI(TAG, "Initializing Shine encoder: %d Hz, %d kbps, mono", 
             MIMI_AUDIO_SAMPLE_RATE, MIMI_MP3_BITRATE);

    s_encoder = shine_initialise(&config);
    if (!s_encoder) {
        ESP_LOGE(TAG, "Failed to initialize Shine encoder");
        return ESP_ERR_NO_MEM;
    }

    s_samples_per_pass = shine_samples_per_pass(s_encoder);
    ESP_LOGI(TAG, "Encoder ready, samples per pass: %d", s_samples_per_pass);

    return ESP_OK;
}

esp_err_t mp3_encode_pcm(const int16_t *pcm_data, size_t pcm_len,
                         uint32_t sample_rate, uint8_t **mp3_out, size_t *mp3_len)
{
    if (!pcm_data || pcm_len == 0 || !mp3_out || !mp3_len) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_encoder) {
        ESP_LOGE(TAG, "Encoder not initialized, call mp3_encoder_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    *mp3_out = NULL;
    *mp3_len = 0;

    TIMING_START();

    size_t pcm_samples = pcm_len / sizeof(int16_t);

    size_t est_mp3_size = (pcm_len / 8) + 8192;
    uint8_t *mp3_buf = heap_caps_malloc(est_mp3_size, MALLOC_CAP_SPIRAM);
    if (!mp3_buf) {
        ESP_LOGE(TAG, "Failed to allocate MP3 buffer");
        return ESP_ERR_NO_MEM;
    }

    size_t mp3_pos = 0;
    size_t pcm_pos = 0;

    int16_t *frame_buf = malloc(s_samples_per_pass * sizeof(int16_t));
    if (!frame_buf) {
        free(mp3_buf);
        return ESP_ERR_NO_MEM;
    }

    while (pcm_pos < pcm_samples) {
        size_t samples_left = pcm_samples - pcm_pos;
        size_t samples_to_encode = samples_left < (size_t)s_samples_per_pass ? samples_left : (size_t)s_samples_per_pass;

        memcpy(frame_buf, pcm_data + pcm_pos, samples_to_encode * sizeof(int16_t));

        if (samples_to_encode < (size_t)s_samples_per_pass) {
            memset(frame_buf + samples_to_encode, 0, (s_samples_per_pass - samples_to_encode) * sizeof(int16_t));
        }

        int written = 0;
        unsigned char *data = shine_encode_buffer_interleaved(s_encoder, frame_buf, &written);

        if (written > 0 && data) {
            if (mp3_pos + written > est_mp3_size) {
                ESP_LOGE(TAG, "MP3 buffer overflow");
                free(frame_buf);
                free(mp3_buf);
                return ESP_ERR_NO_MEM;
            }
            memcpy(mp3_buf + mp3_pos, data, written);
            mp3_pos += written;
        }

        pcm_pos += s_samples_per_pass;
    }

    int written = 0;
    unsigned char *data = shine_flush(s_encoder, &written);
    if (written > 0 && data) {
        if (mp3_pos + written <= est_mp3_size) {
            memcpy(mp3_buf + mp3_pos, data, written);
            mp3_pos += written;
        }
    }

    free(frame_buf);

    ESP_LOGI(TAG, "MP3 encoding complete: %d bytes PCM → %d bytes MP3 (%.1f%% compression)",
             (int)pcm_len, (int)mp3_pos, 100.0f * (1.0f - ((float)mp3_pos / (float)pcm_len)));
    
    TIMING_END_LOG(TAG, "MP3 encode");

    *mp3_out = mp3_buf;
    *mp3_len = mp3_pos;

    return ESP_OK;
}
