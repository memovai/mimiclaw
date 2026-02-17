#include "tts_client.h"
#include "mimi_config.h"
#include "mp3_decoder.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tts";

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    int sample_rate;
} tts_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    tts_buf_t *tb = (tts_buf_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_HEADER && tb) {
        if (strcasecmp(evt->header_key, "X-Sample-Rate") == 0) {
            tb->sample_rate = atoi(evt->header_value);
            ESP_LOGI(TAG, "TTS sample rate from header: %d Hz", tb->sample_rate);
        }
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA && tb) {
        if (tb->len + evt->data_len <= tb->cap) {
            memcpy(tb->buf + tb->len, evt->data, evt->data_len);
            tb->len += evt->data_len;
        } else {
            ESP_LOGW(TAG, "TTS buffer full (%d/%d), dropping %d bytes",
                     (int)tb->len, (int)tb->cap, (int)evt->data_len);
        }
    }
    return ESP_OK;
}

/* Downsample PCM in-place using linear interpolation.
 * Safe for downsampling (out_len < in_len) since writes lag behind reads. */
static size_t resample_pcm(int16_t *buf, size_t in_samples, uint32_t src_rate, uint32_t dst_rate)
{
    if (src_rate == dst_rate || in_samples == 0) return in_samples;

    size_t out_samples = (size_t)((uint64_t)in_samples * dst_rate / src_rate);

    for (size_t j = 0; j < out_samples; j++) {
        uint64_t pos_num = (uint64_t)j * src_rate;
        size_t i0 = (size_t)(pos_num / dst_rate);
        uint32_t frac = (uint32_t)(pos_num % dst_rate);

        size_t i1 = (i0 + 1 < in_samples) ? i0 + 1 : i0;

        int32_t s0 = buf[i0];
        int32_t s1 = buf[i1];
        int32_t out = s0 + (int32_t)((int64_t)(s1 - s0) * frac / (int64_t)dst_rate);
        if (out > 32767)  out = 32767;
        if (out < -32768) out = -32768;
        buf[j] = (int16_t)out;
    }

    return out_samples;
}

esp_err_t tts_synthesize(const char *text, uint8_t *wav_out, size_t wav_out_cap,
                         size_t *wav_len)
{
    if (!text || !wav_out || !wav_len || wav_out_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *wav_len = 0;

    const char *url = (MIMI_SECRET_TTS_URL[0] != '\0')
        ? MIMI_SECRET_TTS_URL
        : NULL;

    char url_buf[128];
    if (!url) {
        snprintf(url_buf, sizeof(url_buf), "http://%s:%s/v1/audio/speech",
                 MIMI_SECRET_STT_HOST, MIMI_SECRET_STT_PORT);
        url = url_buf;
    }

    bool is_https = (strncmp(url, "https://", 8) == 0);

    ESP_LOGI(TAG, "TTS request: \"%s\" → %s", text, url);

    const char *voice = MIMI_SECRET_TTS_VOICE;
    const char *instructions = MIMI_SECRET_TTS_INSTRUCTIONS;

    char json_body[1280];
    int json_len;
    if (voice[0] != '\0' && instructions[0] != '\0') {
        json_len = snprintf(json_body, sizeof(json_body),
            "{\"model\":\"%s\",\"input\":\"%s\",\"voice\":\"%s\","
            "\"instructions\":\"%s\",\"speed\":1.0,\"response_format\":\"pcm\"}",
            MIMI_SECRET_TTS_MODEL, text, voice, instructions);
    } else if (voice[0] != '\0') {
        json_len = snprintf(json_body, sizeof(json_body),
            "{\"model\":\"%s\",\"input\":\"%s\",\"voice\":\"%s\",\"speed\":1.0,\"response_format\":\"pcm\"}",
            MIMI_SECRET_TTS_MODEL, text, voice);
    } else {
        json_len = snprintf(json_body, sizeof(json_body),
            "{\"model\":\"%s\",\"input\":\"%s\",\"speed\":1.0,\"response_format\":\"pcm\"}",
            MIMI_SECRET_TTS_MODEL, text);
    }

    if (json_len >= (int)sizeof(json_body)) {
        ESP_LOGE(TAG, "Text too long for JSON buffer");
        return ESP_ERR_INVALID_SIZE;
    }

    char auth_header[256] = {0};
    if (is_https && MIMI_SECRET_API_KEY[0] != '\0') {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", MIMI_SECRET_API_KEY);
    }

    /* OpenAI TTS PCM is always 24kHz with no header — set as default for HTTPS.
     * Local servers may override via X-Sample-Rate response header. */
    int default_sample_rate = is_https ? 24000 : 16000;

    tts_buf_t tb = {
        .buf = wav_out,
        .len = 0,
        .cap = wav_out_cap,
        .sample_rate = default_sample_rate,
    };

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &tb,
        .timeout_ms = MIMI_HTTP_TIMEOUT_TTS,
        .crt_bundle_attach = is_https ? esp_crt_bundle_attach : NULL,
    };

    esp_err_t err = ESP_FAIL;
    int status = 0;

    for (int attempt = 0; attempt <= MIMI_HTTP_RETRY_COUNT; attempt++) {
        if (attempt > 0) {
            uint32_t delay_ms = MIMI_HTTP_RETRY_DELAY_MS << (attempt - 1);
            if (delay_ms > MIMI_HTTP_RETRY_DELAY_MAX_MS) delay_ms = MIMI_HTTP_RETRY_DELAY_MAX_MS;
            ESP_LOGW(TAG, "TTS retry %d/%d in %"PRIu32"ms...", attempt, MIMI_HTTP_RETRY_COUNT, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            tb.len = 0;
            tb.sample_rate = default_sample_rate;
        }

        int64_t attempt_start = esp_timer_get_time();

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "TTS client init failed (attempt %d)", attempt + 1);
            err = ESP_FAIL;
            continue;
        }

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Connection", "close");
        if (auth_header[0] != '\0') {
            esp_http_client_set_header(client, "Authorization", auth_header);
        }

        err = esp_http_client_open(client, json_len);
        if (err != ESP_OK) {
            int64_t elapsed = (esp_timer_get_time() - attempt_start) / 1000;
            ESP_LOGE(TAG, "TTS connect failed (attempt %d, %lldms): %s", attempt + 1, elapsed, esp_err_to_name(err));
            esp_http_client_cleanup(client);
            continue;
        }

        int written = esp_http_client_write(client, json_body, json_len);
        if (written < 0) {
            ESP_LOGE(TAG, "TTS HTTP write failed (attempt %d)", attempt + 1);
            esp_http_client_cleanup(client);
            err = ESP_FAIL;
            continue;
        }

        int content_length = esp_http_client_fetch_headers(client);
        ESP_LOGI(TAG, "TTS response content_length: %d", content_length);

        if (content_length > 0) {
            int to_read = content_length;
            if ((size_t)to_read > wav_out_cap - tb.len) to_read = wav_out_cap - tb.len;
            int read_len = esp_http_client_read(client, (char *)(wav_out + tb.len), to_read);
            if (read_len > 0) tb.len += read_len;
        } else {
            while (tb.len < tb.cap) {
                int read_len = esp_http_client_read(client, (char *)(wav_out + tb.len),
                                                     tb.cap - tb.len);
                if (read_len <= 0) break;
                tb.len += read_len;
            }
        }

        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        int64_t elapsed = (esp_timer_get_time() - attempt_start) / 1000;

        if (status == 200 && tb.len > 0) {
            ESP_LOGI(TAG, "TTS attempt %d succeeded in %lldms (%d bytes at %dHz)",
                     attempt + 1, elapsed, (int)tb.len, tb.sample_rate);
            err = ESP_OK;
            break;
        }

        if (status != 200) {
            ESP_LOGW(TAG, "TTS HTTP %d (attempt %d, %lldms), retrying...", status, attempt + 1, elapsed);
        } else {
            ESP_LOGW(TAG, "TTS empty response (attempt %d, %lldms), retrying...", attempt + 1, elapsed);
        }
        err = ESP_FAIL;
    }

    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    /* Resample if server sample rate differs from audio HAL rate */
    if (tb.sample_rate != MIMI_AUDIO_SAMPLE_RATE) {
        ESP_LOGI(TAG, "Resampling %dHz → %dHz (%d samples)...",
                 tb.sample_rate, MIMI_AUDIO_SAMPLE_RATE, (int)(tb.len / 2));
        size_t out_samples = resample_pcm(
            (int16_t *)wav_out,
            tb.len / 2,
            (uint32_t)tb.sample_rate,
            MIMI_AUDIO_SAMPLE_RATE
        );
        tb.len = out_samples * 2;
        ESP_LOGI(TAG, "Resampled to %d samples (%d bytes)", (int)out_samples, (int)tb.len);
    }

    *wav_len = tb.len;
    ESP_LOGI(TAG, "TTS complete: %d bytes PCM at %dHz", (int)tb.len, MIMI_AUDIO_SAMPLE_RATE);

    return ESP_OK;
}
