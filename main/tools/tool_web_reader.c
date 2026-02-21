#include "tool_web_reader.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "web_reader";

static char s_jina_key[128] = {0};

#define READER_BUF_SIZE     (32 * 1024)

/* ── Response accumulator ─────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} reader_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    reader_buf_t *rb = (reader_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t needed = rb->len + evt->data_len;
        if (needed < rb->cap) {
            memcpy(rb->data + rb->len, evt->data, evt->data_len);
            rb->len += evt->data_len;
            rb->data[rb->len] = '\0';
        }
    }
    return ESP_OK;
}

/* ── Init ─────────────────────────────────────────────────────── */

esp_err_t tool_web_reader_init(void)
{
    /* Start with build-time default */
    if (MIMI_SECRET_JINA_KEY[0] != '\0') {
        strncpy(s_jina_key, MIMI_SECRET_JINA_KEY, sizeof(s_jina_key) - 1);
    }

    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_JINA, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_jina_key, tmp, sizeof(s_jina_key) - 1);
        }
        nvs_close(nvs);
    }

    if (s_jina_key[0]) {
        ESP_LOGI(TAG, "Web reader initialized (Jina key configured)");
    } else {
        ESP_LOGW(TAG, "No Jina API key. Use CLI: set_jina_key <KEY>");
    }
    return ESP_OK;
}

/* ── URL-encode ───────────────────────────────────────────────── */

static size_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    for (; *src && pos < dst_size - 3; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' ||
            c == ':' || c == '/') {
            dst[pos++] = c;
        } else {
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    dst[pos] = '\0';
    return pos;
}

/* ── Direct HTTPS request ─────────────────────────────────────── */

static esp_err_t reader_direct(const char *url, reader_buf_t *rb)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = rb,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_GET,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    /* Jina Reader headers */
    if (s_jina_key[0]) {
        char auth_header[256];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_jina_key);
        esp_http_client_set_header(client, "Authorization", auth_header);
    }
    esp_http_client_set_header(client, "Accept", "text/plain");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    if (status != 200) {
        ESP_LOGE(TAG, "Jina Reader API returned %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Proxy HTTPS request ──────────────────────────────────────── */

static esp_err_t reader_via_proxy(const char *path, reader_buf_t *rb)
{
    proxy_conn_t *conn = proxy_conn_open("r.jina.ai", 443, 30000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    char header[2048];
    int hlen;

    if (s_jina_key[0]) {
        char auth_header[256];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_jina_key);
        hlen = snprintf(header, sizeof(header),
            "GET %s HTTP/1.1\r\n"
            "Host: r.jina.ai\r\n"
            "Accept: text/plain\r\n"
            "Authorization: %s\r\n"
            "Connection: close\r\n\r\n",
            path, auth_header);
    } else {
        hlen = snprintf(header, sizeof(header),
            "GET %s HTTP/1.1\r\n"
            "Host: r.jina.ai\r\n"
            "Accept: text/plain\r\n"
            "Connection: close\r\n\r\n",
            path);
    }

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    /* Read full response */
    char tmp[4096];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 30000);
        if (n <= 0) break;
        size_t copy = (total + n < rb->cap - 1) ? (size_t)n : rb->cap - 1 - total;
        if (copy > 0) {
            memcpy(rb->data + total, tmp, copy);
            total += copy;
        }
    }
    rb->data[total] = '\0';
    rb->len = total;
    proxy_conn_close(conn);

    /* Check status */
    int status = 0;
    if (total > 5 && strncmp(rb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(rb->data, ' ');
        if (sp) status = atoi(sp + 1);
    }

    /* Strip headers */
    char *body = strstr(rb->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = total - (body - rb->data);
        memmove(rb->data, body, blen);
        rb->len = blen;
        rb->data[rb->len] = '\0';
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Jina Reader API returned %d via proxy", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Execute ──────────────────────────────────────────────────── */

esp_err_t tool_web_reader_execute(const char *input_json, char *output, size_t output_size)
{
    /* Parse input to get URL */
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *url_item = cJSON_GetObjectItem(input, "url");
    if (!url_item || !cJSON_IsString(url_item) || url_item->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'url' field");
        return ESP_ERR_INVALID_ARG;
    }

    const char *target_url = url_item->valuestring;
    ESP_LOGI(TAG, "Reading page: %s", target_url);

    /* Build Jina Reader URL: https://r.jina.ai/<encoded_url> */
    char encoded_url[1024];
    url_encode(target_url, encoded_url, sizeof(encoded_url));

    char full_url[1280];
    snprintf(full_url, sizeof(full_url), "https://r.jina.ai/%s", encoded_url);

    cJSON_Delete(input);

    /* Allocate response buffer from PSRAM */
    reader_buf_t rb = {0};
    rb.data = heap_caps_calloc(1, READER_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!rb.data) {
        snprintf(output, output_size, "Error: Out of memory");
        return ESP_ERR_NO_MEM;
    }
    rb.cap = READER_BUF_SIZE;

    /* Make HTTP request */
    esp_err_t err;
    if (http_proxy_is_enabled()) {
        /* Build path for proxy: /<encoded_url> */
        char path[1280];
        snprintf(path, sizeof(path), "/%s", encoded_url);
        err = reader_via_proxy(path, &rb);
    } else {
        err = reader_direct(full_url, &rb);
    }

    if (err != ESP_OK) {
        free(rb.data);
        snprintf(output, output_size, "Error: Failed to fetch page");
        return err;
    }

    /* Copy result to output, truncating if necessary */
    if (rb.len >= output_size) {
        memcpy(output, rb.data, output_size - 1);
        output[output_size - 1] = '\0';
        ESP_LOGW(TAG, "Page content truncated from %d to %d bytes",
                 (int)rb.len, (int)(output_size - 1));
    } else {
        memcpy(output, rb.data, rb.len + 1);
    }

    free(rb.data);

    ESP_LOGI(TAG, "Page read complete, %d bytes", (int)strlen(output));
    return ESP_OK;
}

esp_err_t tool_web_reader_set_key(const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_JINA, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_jina_key, api_key, sizeof(s_jina_key) - 1);
    ESP_LOGI(TAG, "Jina API key saved");
    return ESP_OK;
}
