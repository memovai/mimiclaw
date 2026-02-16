#include "tool_web_search.h"
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

static const char *TAG = "web_search";

static char s_search_key[128] = {0};
static char s_search_provider[8] = "brave";

#define SEARCH_BUF_SIZE     (16 * 1024)
#define SEARCH_RESULT_COUNT 5

static bool provider_is_zai(void)
{
    return strcmp(s_search_provider, "zai") == 0;
}

/* ── Response accumulator ─────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} search_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    search_buf_t *sb = (search_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t needed = sb->len + evt->data_len;
        if (needed < sb->cap) {
            memcpy(sb->data + sb->len, evt->data, evt->data_len);
            sb->len += evt->data_len;
            sb->data[sb->len] = '\0';
        }
    }
    return ESP_OK;
}

/* ── Init ─────────────────────────────────────────────────────── */

esp_err_t tool_web_search_init(void)
{
    /* Build-time defaults */
    if (MIMI_SECRET_SEARCH_KEY[0] != '\0') {
        strncpy(s_search_key, MIMI_SECRET_SEARCH_KEY, sizeof(s_search_key) - 1);
    }
    strncpy(s_search_provider, MIMI_SECRET_SEARCH_PROVIDER, sizeof(s_search_provider) - 1);
    s_search_provider[sizeof(s_search_provider) - 1] = '\0';

    /* NVS overrides (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_SEARCH, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_search_key, tmp, sizeof(s_search_key) - 1);
        }
        len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_SEARCH_PROVIDER, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_search_provider, tmp, sizeof(s_search_provider) - 1);
            s_search_provider[sizeof(s_search_provider) - 1] = '\0';
        }
        nvs_close(nvs);
    }

    if (s_search_key[0]) {
        ESP_LOGI(TAG, "Web search initialized (provider: %s, key configured)", s_search_provider);
    } else {
        ESP_LOGW(TAG, "No search API key. Use CLI: set_search_key <KEY>");
    }
    return ESP_OK;
}

/* ── Escape string for JSON value ────────────────────────────────── */

static size_t json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t pos = 0;
    for (; *src && pos < dst_size - 2; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[pos++] = '\\';
            dst[pos++] = (char)c;
        } else if (c >= 0x20 && c < 0x7f) {
            dst[pos++] = (char)c;
        }
        /* skip control chars */
    }
    dst[pos] = '\0';
    return pos;
}

/* ── URL-encode a query string ────────────────────────────────── */

static size_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    for (; *src && pos < dst_size - 3; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[pos++] = c;
        } else if (c == ' ') {
            dst[pos++] = '+';
        } else {
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    dst[pos] = '\0';
    return pos;
}

/* ── Format results: Brave ─────────────────────────────────────── */

static void format_results_brave(cJSON *root, char *output, size_t output_size)
{
    cJSON *web = cJSON_GetObjectItem(root, "web");
    if (!web) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    cJSON *results = cJSON_GetObjectItem(web, "results");
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    size_t off = 0;
    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        if (idx >= SEARCH_RESULT_COUNT) break;

        cJSON *title = cJSON_GetObjectItem(item, "title");
        cJSON *url = cJSON_GetObjectItem(item, "url");
        cJSON *desc = cJSON_GetObjectItem(item, "description");

        off += snprintf(output + off, output_size - off,
            "%d. %s\n   %s\n   %s\n\n",
            idx + 1,
            (title && cJSON_IsString(title)) ? title->valuestring : "(no title)",
            (url && cJSON_IsString(url)) ? url->valuestring : "",
            (desc && cJSON_IsString(desc)) ? desc->valuestring : "");

        if (off >= output_size - 1) break;
        idx++;
    }
}

/* ── Format results: Z.ai ──────────────────────────────────────── */

static void format_results_zai(cJSON *root, char *output, size_t output_size)
{
    cJSON *results = cJSON_GetObjectItem(root, "search_result");
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    size_t off = 0;
    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        if (idx >= SEARCH_RESULT_COUNT) break;

        cJSON *title = cJSON_GetObjectItem(item, "title");
        cJSON *link = cJSON_GetObjectItem(item, "link");
        cJSON *content = cJSON_GetObjectItem(item, "content");
        cJSON *publish_date = cJSON_GetObjectItem(item, "publish_date");

        const char *title_s = (title && cJSON_IsString(title)) ? title->valuestring : "(no title)";
        const char *link_s = (link && cJSON_IsString(link)) ? link->valuestring : "";
        const char *content_s = (content && cJSON_IsString(content)) ? content->valuestring : "";
        const char *date_s = (publish_date && cJSON_IsString(publish_date)) ? publish_date->valuestring : "";

        /* Truncate long content so output fits; allow ~400 chars per result */
        size_t content_max = (output_size - off > 420) ? 350 : (output_size - off > 80 ? (size_t)(output_size - off - 70) : 0);
        if (content_max > 0 && content_s[0]) {
            size_t clen = strlen(content_s);
            if (clen > content_max) {
                char buf[360];
                memcpy(buf, content_s, content_max);
                buf[content_max] = '\0';
                off += snprintf(output + off, output_size - off,
                    "%d. %s\n   %s\n   %s...\n\n",
                    idx + 1, title_s, link_s, buf);
            } else {
                off += snprintf(output + off, output_size - off,
                    "%d. %s\n   %s\n   %s%s%s\n\n",
                    idx + 1, title_s, link_s, content_s,
                    date_s[0] ? " " : "", date_s[0] ? date_s : "");
            }
        } else {
            off += snprintf(output + off, output_size - off,
                "%d. %s\n   %s\n   %s\n\n",
                idx + 1, title_s, link_s, content_s);
        }

        if (off >= output_size - 1) break;
        idx++;
    }
}

/* ── Direct HTTPS: Brave (GET) ─────────────────────────────────── */

static esp_err_t search_direct_brave(const char *url, search_buf_t *sb)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = sb,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "X-Subscription-Token", s_search_key);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    if (status != 200) {
        ESP_LOGE(TAG, "Search API returned %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Direct HTTPS: Z.ai (POST + Bearer) ─────────────────────────── */

static esp_err_t search_direct_zai(const char *url, const char *body_json, search_buf_t *sb)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = sb,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", s_search_key);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, body_json, (int)strlen(body_json));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    if (status != 200) {
        ESP_LOGE(TAG, "Z.ai search API returned %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Proxy HTTPS: Brave (GET) ───────────────────────────────────── */

static esp_err_t search_via_proxy_brave(const char *path, search_buf_t *sb)
{
    proxy_conn_t *conn = proxy_conn_open("api.search.brave.com", 443, 15000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "GET %s HTTP/1.1\r\n"
        "Host: api.search.brave.com\r\n"
        "Accept: application/json\r\n"
        "X-Subscription-Token: %s\r\n"
        "Connection: close\r\n\r\n",
        path, s_search_key);

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    char tmp[4096];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 15000);
        if (n <= 0) break;
        size_t copy = (total + n < sb->cap - 1) ? (size_t)n : sb->cap - 1 - total;
        if (copy > 0) {
            memcpy(sb->data + total, tmp, copy);
            total += copy;
        }
    }
    sb->data[total] = '\0';
    sb->len = total;
    proxy_conn_close(conn);

    int status = 0;
    if (total > 5 && strncmp(sb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(sb->data, ' ');
        if (sp) status = atoi(sp + 1);
    }
    char *body = strstr(sb->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = total - (body - sb->data);
        memmove(sb->data, body, blen);
        sb->len = blen;
        sb->data[sb->len] = '\0';
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Search API returned %d via proxy", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Proxy HTTPS: Z.ai (POST + Bearer) ─────────────────────────── */

static esp_err_t search_via_proxy_zai(const char *path, const char *body_json, search_buf_t *sb)
{
    proxy_conn_t *conn = proxy_conn_open(MIMI_ZAI_API_HOST, 443, 15000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    size_t body_len = strlen(body_json);
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: " MIMI_ZAI_API_HOST "\r\n"
        "Accept: application/json\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "Authorization: Bearer %s\r\n"
        "Connection: close\r\n\r\n",
        path, (unsigned)body_len, s_search_key);

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }
    if (proxy_conn_write(conn, body_json, (int)body_len) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    char tmp[4096];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 15000);
        if (n <= 0) break;
        size_t copy = (total + n < sb->cap - 1) ? (size_t)n : sb->cap - 1 - total;
        if (copy > 0) {
            memcpy(sb->data + total, tmp, copy);
            total += copy;
        }
    }
    sb->data[total] = '\0';
    sb->len = total;
    proxy_conn_close(conn);

    int status = 0;
    if (total > 5 && strncmp(sb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(sb->data, ' ');
        if (sp) status = atoi(sp + 1);
    }
    char *resp_body = strstr(sb->data, "\r\n\r\n");
    if (resp_body) {
        resp_body += 4;
        size_t blen = total - (resp_body - sb->data);
        memmove(sb->data, resp_body, blen);
        sb->len = blen;
        sb->data[sb->len] = '\0';
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Z.ai search API returned %d via proxy", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Execute ──────────────────────────────────────────────────── */

esp_err_t tool_web_search_execute(const char *input_json, char *output, size_t output_size)
{
    if (s_search_key[0] == '\0') {
        snprintf(output, output_size, "Error: No search API key configured. Set via set_search_key or MIMI_SECRET_SEARCH_KEY.");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *query = cJSON_GetObjectItem(input, "query");
    if (!query || !cJSON_IsString(query) || query->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'query' field");
        return ESP_ERR_INVALID_ARG;
    }

    const char *q = query->valuestring;
    ESP_LOGI(TAG, "Searching (%s): %s", s_search_provider, q);

    search_buf_t sb = {0};
    sb.data = heap_caps_calloc(1, SEARCH_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!sb.data) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Out of memory");
        return ESP_ERR_NO_MEM;
    }
    sb.cap = SEARCH_BUF_SIZE;

    esp_err_t err;
    if (provider_is_zai()) {
        /* Z.ai: POST with JSON body */
        char escaped[256];
        json_escape(q, escaped, sizeof(escaped));
        char body_buf[512];
        int body_len = snprintf(body_buf, sizeof(body_buf),
            "{\"search_engine\":\"search-prime\",\"search_query\":\"%s\",\"count\":%d}",
            escaped, SEARCH_RESULT_COUNT);
        if (body_len < 0 || (size_t)body_len >= sizeof(body_buf)) {
            free(sb.data);
            cJSON_Delete(input);
            snprintf(output, output_size, "Error: Query too long");
            return ESP_ERR_INVALID_ARG;
        }

        if (http_proxy_is_enabled()) {
            err = search_via_proxy_zai(MIMI_ZAI_WEB_SEARCH_PATH, body_buf, &sb);
        } else {
            char url[128];
            snprintf(url, sizeof(url), "https://" MIMI_ZAI_API_HOST MIMI_ZAI_WEB_SEARCH_PATH);
            err = search_direct_zai(url, body_buf, &sb);
        }
        cJSON_Delete(input);

        if (err != ESP_OK) {
            free(sb.data);
            snprintf(output, output_size, "Error: Search request failed (Z.ai)");
            return err;
        }

        cJSON *root = cJSON_Parse(sb.data);
        free(sb.data);
        if (!root) {
            snprintf(output, output_size, "Error: Failed to parse Z.ai search results");
            return ESP_FAIL;
        }
        /* Check for API error object */
        cJSON *code = cJSON_GetObjectItem(root, "code");
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        if (cJSON_IsNumber(code) && code->valueint != 0) {
            const char *msg_s = (msg && cJSON_IsString(msg)) ? msg->valuestring : "Unknown error";
            snprintf(output, output_size, "Error: Z.ai search failed: %s", msg_s);
            cJSON_Delete(root);
            return ESP_FAIL;
        }
        format_results_zai(root, output, output_size);
        cJSON_Delete(root);
    } else {
        /* Brave: GET with query params */
        char encoded_query[256];
        url_encode(q, encoded_query, sizeof(encoded_query));
        cJSON_Delete(input);

        char path[384];
        snprintf(path, sizeof(path),
                 "/res/v1/web/search?q=%s&count=%d", encoded_query, SEARCH_RESULT_COUNT);

        if (http_proxy_is_enabled()) {
            err = search_via_proxy_brave(path, &sb);
        } else {
            char url[512];
            snprintf(url, sizeof(url), "https://api.search.brave.com%s", path);
            err = search_direct_brave(url, &sb);
        }

        if (err != ESP_OK) {
            free(sb.data);
            snprintf(output, output_size, "Error: Search request failed");
            return err;
        }

        cJSON *root = cJSON_Parse(sb.data);
        free(sb.data);
        if (!root) {
            snprintf(output, output_size, "Error: Failed to parse search results");
            return ESP_FAIL;
        }
        format_results_brave(root, output, output_size);
        cJSON_Delete(root);
    }

    ESP_LOGI(TAG, "Search complete, %d bytes result", (int)strlen(output));
    return ESP_OK;
}

esp_err_t tool_web_search_set_key(const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_SEARCH, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_search_key, api_key, sizeof(s_search_key) - 1);
    ESP_LOGI(TAG, "Search API key saved");
    return ESP_OK;
}

esp_err_t tool_web_search_set_provider(const char *provider)
{
    if (strcmp(provider, "brave") != 0 && strcmp(provider, "zai") != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_SEARCH, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_str(nvs, MIMI_NVS_KEY_SEARCH_PROVIDER, provider);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err == ESP_OK) {
        strncpy(s_search_provider, provider, sizeof(s_search_provider) - 1);
        s_search_provider[sizeof(s_search_provider) - 1] = '\0';
        ESP_LOGI(TAG, "Search provider set to: %s", s_search_provider);
    }
    return err;
}
