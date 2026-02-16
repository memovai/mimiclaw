#include "mp3_decoder.h"
#include "mp3dec.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "mp3_decoder";

static HMP3Decoder s_decoder = NULL;

esp_err_t mp3_decoder_init(void)
{
    if (s_decoder) {
        ESP_LOGW(TAG, "Decoder already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Helix MP3 decoder...");

    s_decoder = MP3InitDecoder();
    if (!s_decoder) {
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Decoder ready");
    return ESP_OK;
}

esp_err_t mp3_decode(const uint8_t *mp3_data, size_t mp3_len,
                     int16_t **pcm_out, size_t *pcm_len, int *sample_rate)
{
    if (!mp3_data || mp3_len == 0 || !pcm_out || !pcm_len) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_decoder) {
        ESP_LOGE(TAG, "Decoder not initialized, call mp3_decoder_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    *pcm_out = NULL;
    *pcm_len = 0;

    TIMING_START();

    size_t est_pcm_size = mp3_len * 10;
    int16_t *pcm_buf = heap_caps_malloc(est_pcm_size, MALLOC_CAP_SPIRAM);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "Failed to allocate PCM buffer");
        return ESP_ERR_NO_MEM;
    }

    size_t pcm_pos = 0;
    const uint8_t *read_ptr = mp3_data;
    int bytes_left = mp3_len;
    int first_frame = 1;

    while (bytes_left > 0) {
        int offset = MP3FindSyncWord((unsigned char *)read_ptr, bytes_left);
        if (offset < 0) {
            ESP_LOGW(TAG, "No sync word found, %d bytes left", bytes_left);
            break;
        }

        read_ptr += offset;
        bytes_left -= offset;

        int16_t *out_ptr = pcm_buf + pcm_pos;
        int samples_decoded = 0;
        
        int err = MP3Decode(s_decoder, (unsigned char **)&read_ptr, &bytes_left, 
                           out_ptr, 0);

        if (err) {
            if (err == ERR_MP3_INDATA_UNDERFLOW) {
                ESP_LOGD(TAG, "End of MP3 data");
                break;
            }
            ESP_LOGW(TAG, "MP3 decode error: %d, skipping frame", err);
            read_ptr++;
            bytes_left--;
            continue;
        }

        MP3FrameInfo frame_info;
        MP3GetLastFrameInfo(s_decoder, &frame_info);

        if (first_frame) {
            if (sample_rate) {
                *sample_rate = frame_info.samprate;
            }
            ESP_LOGI(TAG, "MP3: %d Hz, %d ch, %d bps, %d samples/frame",
                     frame_info.samprate, frame_info.nChans, 
                     frame_info.bitsPerSample, frame_info.outputSamps);
            first_frame = 0;
        }

        samples_decoded = frame_info.outputSamps;

        if (pcm_pos + samples_decoded > est_pcm_size / sizeof(int16_t)) {
            ESP_LOGE(TAG, "PCM buffer overflow");
            break;
        }

        pcm_pos += samples_decoded;
    }

    if (pcm_pos == 0) {
        free(pcm_buf);
        ESP_LOGE(TAG, "No PCM data decoded");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "MP3 decoding complete: %d bytes MP3 → %d samples PCM",
             (int)mp3_len, (int)pcm_pos);
    
    TIMING_END_LOG(TAG, "MP3 decode");

    *pcm_out = pcm_buf;
    *pcm_len = pcm_pos * sizeof(int16_t);

    return ESP_OK;
}
