#include "tts_client.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"

static const char *TAG = "tts";

/* Accumulate response into caller's buffer */
typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} tts_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    tts_buf_t *tb = (tts_buf_t *)evt->user_data;
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

esp_err_t tts_synthesize(const char *text, uint8_t *wav_out, size_t wav_out_cap,
                         size_t *wav_len)
{
    if (!text || !wav_out || !wav_len || wav_out_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *wav_len = 0;

    /* Build URL */
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%s/v1/audio/speech",
             MIMI_SECRET_STT_HOST, MIMI_SECRET_STT_PORT);

    ESP_LOGI(TAG, "TTS request: \"%s\" → %s", text, url);

    /* Build JSON body */
    char json_body[1024];
    int json_len = snprintf(json_body, sizeof(json_body),
        "{\"model\":\"%s\",\"input\":\"%s\",\"speed\":1.0}",
        MIMI_SECRET_TTS_MODEL, text);

    if (json_len >= (int)sizeof(json_body)) {
        ESP_LOGE(TAG, "Text too long for JSON buffer");
        return ESP_ERR_INVALID_SIZE;
    }

    /* Response accumulator — writes directly into caller's buffer */
    tts_buf_t tb = {
        .buf = wav_out,
        .len = 0,
        .cap = wav_out_cap,
    };

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &tb,
        .timeout_ms = 30000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, json_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int written = esp_http_client_write(client, json_body, json_len);
    if (written < 0) {
        ESP_LOGE(TAG, "HTTP write failed");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Fetch response headers */
    int content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "TTS response content_length: %d", content_length);

    /* Read response body */
    if (content_length > 0) {
        int to_read = content_length;
        if ((size_t)to_read > wav_out_cap - tb.len) to_read = wav_out_cap - tb.len;
        int read_len = esp_http_client_read(client, (char *)(wav_out + tb.len), to_read);
        if (read_len > 0) tb.len += read_len;
    } else {
        /* Chunked transfer */
        while (tb.len < tb.cap) {
            int read_len = esp_http_client_read(client, (char *)(wav_out + tb.len),
                                                 tb.cap - tb.len);
            if (read_len <= 0) break;
            tb.len += read_len;
        }
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "TTS response: HTTP %d, %d bytes audio data", status, (int)tb.len);

    if (status != 200) {
        ESP_LOGE(TAG, "TTS server returned HTTP %d", status);
        return ESP_FAIL;
    }

    if (tb.len == 0) {
        ESP_LOGE(TAG, "TTS returned empty response");
        return ESP_FAIL;
    }

    *wav_len = tb.len;
    return ESP_OK;
}
