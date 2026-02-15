#include "tool_web_search.h"
#include "mimi_config.h"
#include "mimi_secrets.h"
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
static char s_volcengine_key[128] = {0};
static char s_volcengine_model[64] = "doubao-seed-1-8-251228";

#define SEARCH_BUF_SIZE     (32 * 1024)
#define SEARCH_RESULT_COUNT 5

bool tool_web_search_is_available(void)
{
    return (s_search_key[0] != '\0' || s_volcengine_key[0] != '\0');
}

const char *tool_web_search_get_provider(void)
{
    if (s_search_key[0]) return "Brave Search";
    if (s_volcengine_key[0]) return "Volcengine Web Search";
    return "none";
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
    ESP_LOGI(TAG, "Initializing web search tool...");

    if (MIMI_SECRET_SEARCH_KEY[0] != '\0') {
        strncpy(s_search_key, MIMI_SECRET_SEARCH_KEY, sizeof(s_search_key) - 1);
        ESP_LOGI(TAG, "Brave Search key from secrets: %d chars", (int)strlen(s_search_key));
    }

    if (MIMI_SECRET_VOLCENGINE_API_KEY[0] != '\0') {
        strncpy(s_volcengine_key, MIMI_SECRET_VOLCENGINE_API_KEY, sizeof(s_volcengine_key) - 1);
        ESP_LOGI(TAG, "Volcengine key from secrets: %d chars", (int)strlen(s_volcengine_key));
    }
    if (MIMI_SECRET_VOLCENGINE_MODEL[0] != '\0') {
        strncpy(s_volcengine_model, MIMI_SECRET_VOLCENGINE_MODEL, sizeof(s_volcengine_model) - 1);
        ESP_LOGI(TAG, "Volcengine model from secrets: %s", s_volcengine_model);
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_SEARCH, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_search_key, tmp, sizeof(s_search_key) - 1);
            ESP_LOGI(TAG, "Brave Search key from NVS: %d chars", (int)strlen(s_search_key));
        }
        nvs_close(nvs);
    } else {
        ESP_LOGI(TAG, "No Brave Search key in NVS (err=%d)", err);
    }

    err = nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        esp_err_t key_err = nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len);
        if (key_err == ESP_OK && tmp[0]) {
            strncpy(s_volcengine_key, tmp, sizeof(s_volcengine_key) - 1);
            ESP_LOGI(TAG, "Volcengine key from NVS: %d chars", (int)strlen(s_volcengine_key));
        }
        len = sizeof(s_volcengine_model);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_MODEL, s_volcengine_model, &len) == ESP_OK) {
            ESP_LOGI(TAG, "Volcengine model from NVS: %s", s_volcengine_model);
        }
        nvs_close(nvs);
    } else {
        ESP_LOGI(TAG, "No NVS LLM config (err=%d), using secrets", err);
    }

    ESP_LOGI(TAG, "Final state: Brave=%s, Volcengine=%s",
             s_search_key[0] ? "configured" : "none",
             s_volcengine_key[0] ? "configured" : "none");

    if (s_search_key[0]) {
        ESP_LOGI(TAG, "Web search initialized (Brave Search key configured)");
    } else if (s_volcengine_key[0]) {
        ESP_LOGI(TAG, "Web search initialized (Volcengine fallback enabled)");
    } else {
        ESP_LOGW(TAG, "No search API key. Use CLI: set_search_key <KEY>");
    }
    return ESP_OK;
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

/* ── Format results as readable text ──────────────────────────── */

static void format_results(cJSON *root, char *output, size_t output_size)
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

/* ── Direct HTTPS request ─────────────────────────────────────── */

static esp_err_t search_direct(const char *url, search_buf_t *sb)
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

/* ── Proxy HTTPS request ──────────────────────────────────────── */

static esp_err_t search_via_proxy(const char *path, search_buf_t *sb)
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

    /* Read full response */
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

    /* Check status */
    int status = 0;
    if (total > 5 && strncmp(sb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(sb->data, ' ');
        if (sp) status = atoi(sp + 1);
    }

    /* Strip headers */
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

/* ── Volcengine Web Search ────────────────────────────────────── */

static esp_err_t volcengine_web_search(const char *query, search_buf_t *sb)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", s_volcengine_model);

    cJSON *input = cJSON_CreateArray();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", query);
    cJSON_AddItemToArray(input, msg);
    cJSON_AddItemToObject(body, "input", input);

    cJSON *tools = cJSON_CreateArray();
    cJSON *web_search_tool = cJSON_CreateObject();
    cJSON_AddStringToObject(web_search_tool, "type", "web_search");
    cJSON_AddNumberToObject(web_search_tool, "max_keyword", 2);
    cJSON_AddItemToArray(tools, web_search_tool);
    cJSON_AddItemToObject(body, "tools", tools);

    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Volcengine web search request: %s", query);

    char url[256];
    snprintf(url, sizeof(url), "%s%s", MIMI_VOLCENGINE_API_URL, MIMI_VOLCENGINE_API_PATH);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = sb,
        .timeout_ms = 60000,
        .buffer_size = 8192,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(post_data);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth[192];
    snprintf(auth, sizeof(auth), "Bearer %s", s_volcengine_key);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(post_data);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Volcengine API returned %d, response: %s", status, sb->data);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Volcengine API response: %d bytes", (int)sb->len);
    return ESP_OK;
}

static void format_volcengine_results(cJSON *root, char *output, size_t output_size)
{
    cJSON *output_arr = cJSON_GetObjectItem(root, "output");
    if (!output_arr || !cJSON_IsArray(output_arr)) {
        ESP_LOGW(TAG, "No 'output' array in response");
        snprintf(output, output_size, "No results from Volcengine.");
        return;
    }

    ESP_LOGI(TAG, "Parsing output array with %d items", cJSON_GetArraySize(output_arr));

    size_t off = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, output_arr) {
        cJSON *type = cJSON_GetObjectItem(item, "type");
        if (!type || !cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Item has no type");
            continue;
        }

        ESP_LOGI(TAG, "Output item type: %s", type->valuestring);

        if (strcmp(type->valuestring, "message") == 0) {
            cJSON *content = cJSON_GetObjectItem(item, "content");
            if (content && cJSON_IsArray(content)) {
                ESP_LOGI(TAG, "Message has %d content blocks", cJSON_GetArraySize(content));
                cJSON *block;
                cJSON_ArrayForEach(block, content) {
                    cJSON *btype = cJSON_GetObjectItem(block, "type");
                    if (btype && cJSON_IsString(btype)) {
                        ESP_LOGI(TAG, "Content block type: %s", btype->valuestring);
                        if (strcmp(btype->valuestring, "output_text") == 0) {
                            cJSON *text = cJSON_GetObjectItem(block, "text");
                            if (text && cJSON_IsString(text)) {
                                size_t tlen = strlen(text->valuestring);
                                ESP_LOGI(TAG, "Found output_text: %d chars", (int)tlen);
                                if (off + tlen < output_size - 1) {
                                    memcpy(output + off, text->valuestring, tlen);
                                    off += tlen;
                                }
                            }
                        }
                    }
                }
            }
        } else if (strcmp(type->valuestring, "web_search_call") == 0) {
            cJSON *action = cJSON_GetObjectItem(item, "action");
            if (action && cJSON_IsString(action)) {
                ESP_LOGI(TAG, "Web search call action: %s", action->valuestring);
            }
        }
    }
    output[off] = '\0';

    ESP_LOGI(TAG, "Formatted result: %d bytes", (int)off);

    if (off == 0) {
        snprintf(output, output_size, "No search results found.");
    }
}

/* ── Execute ──────────────────────────────────────────────────── */

esp_err_t tool_web_search_execute(const char *input_json, char *output, size_t output_size)
{
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

    const char *query_str = query->valuestring;
    ESP_LOGI(TAG, "Searching: %s", query_str);

    search_buf_t sb = {0};
    sb.data = heap_caps_calloc(1, SEARCH_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!sb.data) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Out of memory");
        return ESP_ERR_NO_MEM;
    }
    sb.cap = SEARCH_BUF_SIZE;

    esp_err_t err = ESP_FAIL;
    bool used_volcengine = false;

    ESP_LOGI(TAG, "Search keys: Brave=%s, Volcengine=%s",
             s_search_key[0] ? "configured" : "none",
             s_volcengine_key[0] ? "configured" : "none");

    if (s_search_key[0]) {
        char encoded_query[256];
        url_encode(query_str, encoded_query, sizeof(encoded_query));

        char path[384];
        snprintf(path, sizeof(path),
                 "/res/v1/web/search?q=%s&count=%d", encoded_query, SEARCH_RESULT_COUNT);

        if (http_proxy_is_enabled()) {
            err = search_via_proxy(path, &sb);
        } else {
            char url[512];
            snprintf(url, sizeof(url), "https://api.search.brave.com%s", path);
            err = search_direct(url, &sb);
        }

        if (err == ESP_OK) {
            cJSON *root = cJSON_Parse(sb.data);
            if (root) {
                format_results(root, output, output_size);
                cJSON_Delete(root);
            } else {
                err = ESP_FAIL;
            }
        }
    }

    if (err != ESP_OK && s_volcengine_key[0]) {
        ESP_LOGI(TAG, "Brave Search failed, trying Volcengine web search...");
        memset(sb.data, 0, sb.cap);
        sb.len = 0;

        err = volcengine_web_search(query_str, &sb);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Parsing Volcengine response...");
            cJSON *root = cJSON_Parse(sb.data);
            if (root) {
                format_volcengine_results(root, output, output_size);
                cJSON_Delete(root);
            } else {
                ESP_LOGE(TAG, "Failed to parse JSON response: %.200s", sb.data);
                snprintf(output, output_size, "Error: Failed to parse search results");
            }
            used_volcengine = true;
        }
    }

    cJSON_Delete(input);
    free(sb.data);

    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: Search request failed (no API key available)");
        return err;
    }

    ESP_LOGI(TAG, "Search complete (%s), %d bytes result",
             used_volcengine ? "Volcengine" : "Brave", (int)strlen(output));
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

esp_err_t tool_web_search_set_volcengine_key(const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_volcengine_key, api_key, sizeof(s_volcengine_key) - 1);
    ESP_LOGI(TAG, "Volcengine API key saved");
    return ESP_OK;
}

esp_err_t tool_web_search_set_volcengine_model(const char *model)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_MODEL, model));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_volcengine_model, model, sizeof(s_volcengine_model) - 1);
    ESP_LOGI(TAG, "Volcengine model saved: %s", model);
    return ESP_OK;
}
