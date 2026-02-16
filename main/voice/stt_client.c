#include "stt_client.h"
#include "mimi_config.h"
#include "mp3_encoder.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

static const char *TAG = "stt";

/* ─── Response accumulator ─── */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} resp_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb) {
        if (rb->len + evt->data_len < rb->cap) {
            memcpy(rb->buf + rb->len, evt->data, evt->data_len);
            rb->len += evt->data_len;
            rb->buf[rb->len] = '\0';
        }
    }
    return ESP_OK;
}

esp_err_t stt_transcribe(const int16_t *pcm_data, size_t pcm_len,
                         char *text_out, size_t text_size)
{
    if (!pcm_data || pcm_len == 0 || !text_out || text_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    text_out[0] = '\0';

    ESP_LOGI(TAG, "Encoding %d bytes PCM to MP3...", (int)pcm_len);
    
    uint8_t *mp3_data = NULL;
    size_t mp3_len = 0;
    esp_err_t enc_err = mp3_encode_pcm(pcm_data, pcm_len, MIMI_AUDIO_SAMPLE_RATE, &mp3_data, &mp3_len);
    if (enc_err != ESP_OK) {
        ESP_LOGE(TAG, "MP3 encoding failed: %s", esp_err_to_name(enc_err));
        return enc_err;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%s/v1/audio/transcriptions",
             MIMI_SECRET_STT_HOST, MIMI_SECRET_STT_PORT);

    ESP_LOGI(TAG, "STT request: %d bytes MP3 → %s", (int)mp3_len, url);

    const char *boundary = "----MimiSTTBoundary9876";

    const char *model = MIMI_SECRET_STT_MODEL;

    char part1_hdr[256];
    int part1_hdr_len = snprintf(part1_hdr, sizeof(part1_hdr),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.mp3\"\r\n"
        "Content-Type: audio/mpeg\r\n\r\n",
        boundary);

    char part2[256];
    int part2_len = snprintf(part2, sizeof(part2),
        "\r\n--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "%s\r\n"
        "--%s--\r\n",
        boundary, model, boundary);

    size_t body_size = part1_hdr_len + mp3_len + part2_len;

    uint8_t *body = heap_caps_malloc(body_size, MALLOC_CAP_SPIRAM);
    if (!body) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for multipart body", (int)body_size);
        free(mp3_data);
        return ESP_ERR_NO_MEM;
    }

    size_t pos = 0;
    memcpy(body + pos, part1_hdr, part1_hdr_len);
    pos += part1_hdr_len;

    memcpy(body + pos, mp3_data, mp3_len);
    pos += mp3_len;
    
    free(mp3_data);

    memcpy(body + pos, part2, part2_len);
    pos += part2_len;

    /* Response buffer */
    resp_buf_t rb = {
        .buf = malloc(2048),
        .len = 0,
        .cap = 2048,
    };
    if (!rb.buf) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    rb.buf[0] = '\0';

    /* Content-Type header */
    char content_type[80];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);

    /* HTTP POST */
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &rb,
        .timeout_ms = MIMI_HTTP_TIMEOUT_STT,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(body);
        free(rb.buf);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", content_type);

    esp_err_t err = esp_http_client_open(client, (int)body_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(body);
        free(rb.buf);
        return err;
    }

    /* Write body in chunks (esp_http_client_open + write for large bodies) */
    size_t written = 0;
    while (written < body_size) {
        size_t chunk = body_size - written;
        if (chunk > 4096) chunk = 4096;
        int w = esp_http_client_write(client, (const char *)(body + written), (int)chunk);
        if (w < 0) {
            ESP_LOGE(TAG, "HTTP write failed at offset %d", (int)written);
            esp_http_client_cleanup(client);
            free(body);
            free(rb.buf);
            return ESP_FAIL;
        }
        written += w;
    }
    free(body);

    /* Fetch response */
    int content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "Response content_length: %d", content_length);

    /* Read response body (event handler accumulates it, but read explicitly too) */
    if (content_length > 0) {
        int to_read = content_length;
        if ((size_t)to_read > rb.cap - rb.len - 1) to_read = rb.cap - rb.len - 1;
        int read_len = esp_http_client_read(client, rb.buf + rb.len, to_read);
        if (read_len > 0) {
            rb.len += read_len;
            rb.buf[rb.len] = '\0';
        }
    } else {
        /* chunked transfer — read until done */
        while (1) {
            int read_len = esp_http_client_read(client, rb.buf + rb.len, rb.cap - rb.len - 1);
            if (read_len <= 0) break;
            rb.len += read_len;
            rb.buf[rb.len] = '\0';
        }
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "STT response (HTTP %d, %d bytes): %s", status, (int)rb.len, rb.buf);

    if (status != 200) {
        ESP_LOGE(TAG, "STT server returned HTTP %d", status);
        free(rb.buf);
        return ESP_FAIL;
    }

    /* Parse JSON response: {"text": "..."} */
    cJSON *root = cJSON_Parse(rb.buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse STT JSON response");
        free(rb.buf);
        return ESP_FAIL;
    }

    cJSON *text_field = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text_field) && text_field->valuestring) {
        strncpy(text_out, text_field->valuestring, text_size - 1);
        text_out[text_size - 1] = '\0';
        ESP_LOGI(TAG, "Transcription: \"%s\"", text_out);
    } else {
        ESP_LOGW(TAG, "No 'text' field in STT response");
    }

    cJSON_Delete(root);
    free(rb.buf);

    return ESP_OK;
}
