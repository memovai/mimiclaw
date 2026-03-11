#include "llm_proxy.h"
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

static const char *TAG = "llm";

#define LLM_API_KEY_MAX_LEN 320
#define LLM_MODEL_MAX_LEN   64
#define LLM_API_BASE_MAX_LEN 256
#define LLM_HOST_MAX_LEN    128
#define LLM_PATH_MAX_LEN    128
#define LLM_DUMP_MAX_BYTES   (16 * 1024)
#define LLM_DUMP_CHUNK_BYTES 320

static char s_api_key[LLM_API_KEY_MAX_LEN] = {0};
static char s_model[LLM_MODEL_MAX_LEN] = MIMI_LLM_DEFAULT_MODEL;
static char s_model_id[LLM_MODEL_MAX_LEN] = {0};
static char s_provider[16] = MIMI_LLM_PROVIDER_DEFAULT;
static char s_api_base[LLM_API_BASE_MAX_LEN] = {0};

typedef enum {
    LLM_PROTOCOL_ANTHROPIC = 0,
    LLM_PROTOCOL_OPENAI = 1,
} llm_protocol_t;

static llm_protocol_t s_protocol = LLM_PROTOCOL_ANTHROPIC;
static bool s_api_tls = true;
static char s_api_host[LLM_HOST_MAX_LEN] = {0};
static uint16_t s_api_port = 443;
static char s_api_base_path[LLM_PATH_MAX_LEN] = {0};
static char s_api_req_path[LLM_PATH_MAX_LEN + 32] = {0};
static char s_api_host_header[LLM_HOST_MAX_LEN + 8] = {0};
static char s_api_url[LLM_API_BASE_MAX_LEN + 64] = {0};
static bool s_logged_proxy_bypass_warning = false;

static const char *llm_protocol_name(llm_protocol_t p)
{
    return (p == LLM_PROTOCOL_OPENAI) ? "openai" : "anthropic";
}

static void llm_log_payload(const char *label, const char *payload)
{
    if (!payload) {
        ESP_LOGI(TAG, "%s: <null>", label);
        return;
    }

    size_t total = strlen(payload);
#if MIMI_LLM_LOG_VERBOSE_PAYLOAD
    size_t shown = total > LLM_DUMP_MAX_BYTES ? LLM_DUMP_MAX_BYTES : total;
    ESP_LOGI(TAG, "%s (%u bytes)%s",
             label,
             (unsigned)total,
             (shown < total) ? " [truncated]" : "");

    char chunk[LLM_DUMP_CHUNK_BYTES + 1];
    for (size_t off = 0; off < shown; off += LLM_DUMP_CHUNK_BYTES) {
        size_t n = shown - off;
        if (n > LLM_DUMP_CHUNK_BYTES) {
            n = LLM_DUMP_CHUNK_BYTES;
        }
        memcpy(chunk, payload + off, n);
        chunk[n] = '\0';
        ESP_LOGI(TAG, "%s[%u]: %s", label, (unsigned)off, chunk);
    }
#else
    if (MIMI_LLM_LOG_PREVIEW_BYTES > 0) {
        size_t shown = total > MIMI_LLM_LOG_PREVIEW_BYTES ? MIMI_LLM_LOG_PREVIEW_BYTES : total;
        char preview[MIMI_LLM_LOG_PREVIEW_BYTES + 1];
        memcpy(preview, payload, shown);
        preview[shown] = '\0';
        for (size_t i = 0; i < shown; i++) {
            if (preview[i] == '\n' || preview[i] == '\r' || preview[i] == '\t') {
                preview[i] = ' ';
            }
        }
        ESP_LOGI(TAG, "%s (%u bytes): %s%s",
                 label,
                 (unsigned)total,
                 preview,
                 (shown < total) ? " ..." : "");
    } else {
        ESP_LOGI(TAG, "%s (%u bytes)", label, (unsigned)total);
    }
#endif
}

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strnlen(src, dst_size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ── Response buffer ──────────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} resp_buf_t;

static esp_err_t resp_buf_init(resp_buf_t *rb, size_t initial_cap)
{
    rb->data = heap_caps_calloc(1, initial_cap, MALLOC_CAP_SPIRAM);
    if (!rb->data) return ESP_ERR_NO_MEM;
    rb->len = 0;
    rb->cap = initial_cap;
    return ESP_OK;
}

static esp_err_t resp_buf_append(resp_buf_t *rb, const char *data, size_t len)
{
    while (rb->len + len >= rb->cap) {
        size_t new_cap = rb->cap * 2;
        char *tmp = heap_caps_realloc(rb->data, new_cap, MALLOC_CAP_SPIRAM);
        if (!tmp) return ESP_ERR_NO_MEM;
        rb->data = tmp;
        rb->cap = new_cap;
    }
    memcpy(rb->data + rb->len, data, len);
    rb->len += len;
    rb->data[rb->len] = '\0';
    return ESP_OK;
}

static void resp_buf_free(resp_buf_t *rb)
{
    free(rb->data);
    rb->data = NULL;
    rb->len = 0;
    rb->cap = 0;
}

/* ── Chunked transfer encoding decoder ───────────────────────── */

static void resp_buf_decode_chunked(resp_buf_t *rb)
{
    if (!rb->data || rb->len == 0) return;

    /* Quick check: if body starts with '{' or '[', it's not chunked */
    size_t i = 0;
    while (i < rb->len && (rb->data[i] == ' ' || rb->data[i] == '\t')) i++;
    if (i < rb->len && (rb->data[i] == '{' || rb->data[i] == '[')) return;

    /* Try to decode chunked encoding in-place */
    char *src = rb->data;
    char *dst = rb->data;
    char *end = rb->data + rb->len;

    while (src < end) {
        /* Parse hex chunk size */
        char *line_end = strstr(src, "\r\n");
        if (!line_end) break;

        unsigned long chunk_size = strtoul(src, NULL, 16);
        if (chunk_size == 0) break;  /* terminal chunk */

        src = line_end + 2;  /* skip past \r\n after size */

        if (src + chunk_size > end) {
            /* Incomplete chunk, copy what we have */
            size_t avail = end - src;
            memmove(dst, src, avail);
            dst += avail;
            break;
        }

        memmove(dst, src, chunk_size);
        dst += chunk_size;
        src += chunk_size;

        /* Skip trailing \r\n after chunk data */
        if (src + 2 <= end && src[0] == '\r' && src[1] == '\n') {
            src += 2;
        }
    }

    rb->len = dst - rb->data;
    rb->data[rb->len] = '\0';
}

/* ── HTTP event handler (for esp_http_client direct path) ─────── */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        resp_buf_append(rb, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

/* ── Protocol config ─────────────────────────────────────────── */

typedef struct {
    llm_protocol_t protocol;
    const char *label;   /* "openai" */
    const char *prefix;  /* "openai/" */
    const char *suffix;  /* "/chat/completions" */
    const char *base;    /* Default API base */
} llm_proto_cfg_t;

static const llm_proto_cfg_t PROTO_MAP[] = {
    {LLM_PROTOCOL_OPENAI,    "openai",    "openai/",    "/chat/completions", MIMI_LLM_API_BASE_OPENAI},
    {LLM_PROTOCOL_ANTHROPIC, "anthropic", "anthropic/", "/messages",        MIMI_LLM_API_BASE_ANTHROPIC}
};

static const llm_proto_cfg_t* get_current_proto(void) {
    return &PROTO_MAP[s_protocol == LLM_PROTOCOL_OPENAI ? 0 : 1];
}

/* ── Helpers ─────────────────────────────────────────────────── */

static bool llm_protocol_is_openai(void) {
    return s_protocol == LLM_PROTOCOL_OPENAI;
}

/* Validate api_base format without modifying global state */
static esp_err_t llm_validate_api_base(const char *api_base) {
    if (!api_base || api_base[0] == '\0') return ESP_ERR_INVALID_ARG;

    /* Check for valid scheme */
    const char *p;
    if (strncmp(api_base, "https://", 8) == 0) {
        p = api_base + 8;
    } else if (strncmp(api_base, "http://", 7) == 0) {
        p = api_base + 7;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    /* Basic format validation - ensure there's content after the scheme */
    if (p[0] == '\0' || p[0] == '/' || p[0] == ':') {
        return ESP_ERR_INVALID_ARG;
    }

    /* Check for valid host part (before colon or slash) */
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && slash && colon > slash) colon = NULL; /* Colon is part of path */

    const char *host_end = colon ? colon : (slash ? slash : p + strlen(p));
    if (host_end == p) return ESP_ERR_INVALID_ARG; /* Empty host */

    /* Validate port if present */
    if (colon) {
        char *endptr;
        long port = strtol(colon + 1, &endptr, 10);
        if (endptr == colon + 1 || (*endptr != '\0' && *endptr != '/') ||
            port < 1 || port > 65535) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

/* Parse api_base: scheme (http/https), host[:port], optional base path. */
static esp_err_t llm_parse_api_base(const char *api_base) {
    if (!api_base || api_base[0] == '\0') return ESP_ERR_INVALID_ARG;

    const char *p;
    if (strncmp(api_base, "https://", 8) == 0) {
        s_api_tls = true; p = api_base + 8; s_api_port = 443;
    } else if (strncmp(api_base, "http://", 7) == 0) {
        s_api_tls = false; p = api_base + 7; s_api_port = 80;
    } else return ESP_ERR_INVALID_ARG;

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && slash && colon > slash) colon = NULL; /* Colon is part of path */

    const char *host_end = colon ? colon : (slash ? slash : p + strlen(p));
    snprintf(s_api_host, sizeof(s_api_host), "%.*s", (int)(host_end - p), p);

    if (colon) {
        char *endptr;
        long port = strtol(colon + 1, &endptr, 10);
        if (endptr != colon + 1 && (*endptr == '\0' || *endptr == '/') &&
            port >= 1 && port <= 65535) {
            s_api_port = (uint16_t)port;
        }
        /* If port parsing fails, keep the default port (443 for HTTPS, 80 for HTTP) */
    }

    s_api_base_path[0] = '\0';
    if (slash) {
        safe_copy(s_api_base_path, sizeof(s_api_base_path), slash);
        size_t len = strlen(s_api_base_path);
        while (len > 0 && s_api_base_path[len - 1] == '/') s_api_base_path[--len] = '\0';
    }
    return ESP_OK;
}

/* Build derived request path, Host header, and full URL strings. */
static void llm_build_request_targets(void) {
    const llm_proto_cfg_t *cfg = get_current_proto();

    snprintf(s_api_req_path, sizeof(s_api_req_path), "%s%s", s_api_base_path, cfg->suffix);
    if (s_api_req_path[0] == '\0') strcpy(s_api_req_path, "/");

    bool is_std = (s_api_tls && s_api_port == 443) || (!s_api_tls && s_api_port == 80);
    if (is_std) {
        snprintf(s_api_host_header, sizeof(s_api_host_header), "%s", s_api_host);
    } else {
        snprintf(s_api_host_header, sizeof(s_api_host_header), "%s:%u", s_api_host, s_api_port);
    }

    snprintf(s_api_url, sizeof(s_api_url), "%s://%s%s", 
             s_api_tls ? "https" : "http", s_api_host_header, s_api_req_path);
}

/* ── Derived config ──────────────────────────────────────────── */

static void llm_recompute_effective_config(void) {
    /* Determine protocol + model_id (prefix overrides provider), and update request targets. */
    s_logged_proxy_bypass_warning = false;  /* Reset warning flag when config changes */
    s_protocol = (strcmp(s_provider, "openai") == 0) ? LLM_PROTOCOL_OPENAI : LLM_PROTOCOL_ANTHROPIC;
    const char *model_id = s_model;

    for (int i = 0; i < 2; i++) {
        size_t len = strlen(PROTO_MAP[i].prefix);
        if (strncmp(s_model, PROTO_MAP[i].prefix, len) == 0 && s_model[len] != '\0') {
            s_protocol = PROTO_MAP[i].protocol;
            model_id = s_model + len;
            break;
        }
    }
    safe_copy(s_model_id, sizeof(s_model_id), model_id);

    const char *default_base = get_current_proto()->base;
    const char *base = (s_api_base[0] != '\0') ? s_api_base : default_base;

    if (llm_parse_api_base(base) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse API base: %s. Using default.", base);
        llm_parse_api_base(default_base);
    }

    llm_build_request_targets();

    ESP_LOGI(TAG, "Configured: Protocol=%s, Model=%s, URL=%s", 
             get_current_proto()->label, s_model_id, s_api_url);
}

esp_err_t llm_proxy_init(void)
{
    /* Start with build-time defaults */
    if (MIMI_SECRET_API_KEY[0] != '\0') {
        safe_copy(s_api_key, sizeof(s_api_key), MIMI_SECRET_API_KEY);
    }
    if (MIMI_SECRET_API_BASE[0] != '\0') {
        safe_copy(s_api_base, sizeof(s_api_base), MIMI_SECRET_API_BASE);
    }
    if (MIMI_SECRET_MODEL[0] != '\0') {
        safe_copy(s_model, sizeof(s_model), MIMI_SECRET_MODEL);
    }
    if (MIMI_SECRET_MODEL_PROVIDER[0] != '\0') {
        safe_copy(s_provider, sizeof(s_provider), MIMI_SECRET_MODEL_PROVIDER);
    }

    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[LLM_API_KEY_MAX_LEN] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            safe_copy(s_api_key, sizeof(s_api_key), tmp);
        }
        char base_tmp[LLM_API_BASE_MAX_LEN] = {0};
        len = sizeof(base_tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_BASE, base_tmp, &len) == ESP_OK && base_tmp[0]) {
            safe_copy(s_api_base, sizeof(s_api_base), base_tmp);
        }
        char model_tmp[LLM_MODEL_MAX_LEN] = {0};
        len = sizeof(model_tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_MODEL, model_tmp, &len) == ESP_OK && model_tmp[0]) {
            safe_copy(s_model, sizeof(s_model), model_tmp);
        }
        char provider_tmp[16] = {0};
        len = sizeof(provider_tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_PROVIDER, provider_tmp, &len) == ESP_OK && provider_tmp[0]) {
            safe_copy(s_provider, sizeof(s_provider), provider_tmp);
        }
        nvs_close(nvs);
    }

    llm_recompute_effective_config();

    if (s_api_key[0] == '\0') {
        ESP_LOGW(TAG, "No API key. Use CLI: set_api_key <KEY>");
    }
    return ESP_OK;
}

/* ── Direct path: esp_http_client ───────────────────────────── */

static esp_err_t llm_http_direct(const char *post_data, resp_buf_t *rb, int *out_status)
{
    esp_http_client_config_t config = {
        .url = s_api_url,
        .event_handler = http_event_handler,
        .user_data = rb,
        .timeout_ms = 120 * 1000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (llm_protocol_is_openai()) {
        if (s_api_key[0]) {
            char auth[LLM_API_KEY_MAX_LEN + 16];
            snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
            esp_http_client_set_header(client, "Authorization", auth);
        }
    } else {
        esp_http_client_set_header(client, "x-api-key", s_api_key);
        esp_http_client_set_header(client, "anthropic-version", MIMI_LLM_API_VERSION);
    }
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

/* ── Proxy path: manual HTTP over CONNECT tunnel ────────────── */

static esp_err_t llm_http_via_proxy(const char *post_data, resp_buf_t *rb, int *out_status)
{
    proxy_conn_t *conn = proxy_conn_open(s_api_host, s_api_port, 30000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    /* Build request headers */
    char h[1024];
    int off = snprintf(h, sizeof(h), "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n", 
                       s_api_req_path, s_api_host_header);

    if (llm_protocol_is_openai()) {
        off += snprintf(h + off, sizeof(h) - off, "Authorization: Bearer %s\r\n", s_api_key);
    } else {
        off += snprintf(h + off, sizeof(h) - off, "x-api-key: %s\r\nanthropic-version: %s\r\n", 
                        s_api_key, MIMI_LLM_API_VERSION);
    }

    off += snprintf(h + off, sizeof(h) - off, "Content-Length: %zu\r\nConnection: close\r\n\r\n", strlen(post_data));

    /* Send */
    if (off >= sizeof(h) || proxy_conn_write(conn, h, off) < 0 || 
        proxy_conn_write(conn, post_data, strlen(post_data)) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    /* Receive full response */
    char tmp[1024];
    int n;
    while ((n = proxy_conn_read(conn, tmp, sizeof(tmp), 120000)) > 0) {
        if (resp_buf_append(rb, tmp, n) != ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    proxy_conn_close(conn);

    /* Parse status */
    *out_status = (rb->len > 12 && strncmp(rb->data, "HTTP/", 5) == 0) ? atoi(rb->data + 9) : 0;

    /* Strip headers */
    char *body = strstr(rb->data, "\r\n\r\n");
    if (body) {
        body += 4;
        rb->len -= (body - rb->data);
        memmove(rb->data, body, rb->len);
        rb->data[rb->len] = '\0';
    }

    resp_buf_decode_chunked(rb);
    return ESP_OK;
}

static esp_err_t llm_http_call(const char *post_data, resp_buf_t *rb, int *out_status)
{
    if (http_proxy_is_enabled()) {
        if (s_api_tls) {
            return llm_http_via_proxy(post_data, rb, out_status);
        }
        if (!s_logged_proxy_bypass_warning) {
            ESP_LOGW(TAG, "Proxy configured but api_base is http; bypassing proxy");
            s_logged_proxy_bypass_warning = true;
        }
    }
    return llm_http_direct(post_data, rb, out_status);
}

static cJSON *convert_tools_openai(const char *tools_json)
{
    if (!tools_json) return NULL;
    cJSON *arr = cJSON_Parse(tools_json);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return NULL;
    }
    cJSON *out = cJSON_CreateArray();
    cJSON *tool;
    cJSON_ArrayForEach(tool, arr) {
        cJSON *name = cJSON_GetObjectItem(tool, "name");
        cJSON *desc = cJSON_GetObjectItem(tool, "description");
        cJSON *schema = cJSON_GetObjectItem(tool, "input_schema");
        if (!name || !cJSON_IsString(name)) continue;

        cJSON *func = cJSON_CreateObject();
        cJSON_AddStringToObject(func, "name", name->valuestring);
        if (desc && cJSON_IsString(desc)) {
            cJSON_AddStringToObject(func, "description", desc->valuestring);
        }
        if (schema) {
            cJSON_AddItemToObject(func, "parameters", cJSON_Duplicate(schema, 1));
        }

        cJSON *wrap = cJSON_CreateObject();
        cJSON_AddStringToObject(wrap, "type", "function");
        cJSON_AddItemToObject(wrap, "function", func);
        cJSON_AddItemToArray(out, wrap);
    }
    cJSON_Delete(arr);
    return out;
}

static cJSON *convert_messages_openai(const char *system_prompt, cJSON *messages)
{
    cJSON *out = cJSON_CreateArray();
    if (system_prompt && system_prompt[0]) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", system_prompt);
        cJSON_AddItemToArray(out, sys);
    }

    if (!messages || !cJSON_IsArray(messages)) return out;

    cJSON *msg;
    cJSON_ArrayForEach(msg, messages) {
        cJSON *role = cJSON_GetObjectItem(msg, "role");
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (!role || !cJSON_IsString(role)) continue;

        if (content && cJSON_IsString(content)) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", role->valuestring);
            cJSON_AddStringToObject(m, "content", content->valuestring);
            cJSON_AddItemToArray(out, m);
            continue;
        }

        if (!content || !cJSON_IsArray(content)) continue;

        if (strcmp(role->valuestring, "assistant") == 0) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", "assistant");

            /* collect text */
            char *text_buf = NULL;
            size_t off = 0;
            cJSON *block;
            cJSON *tool_calls = NULL;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        size_t tlen = strlen(text->valuestring);
                        char *tmp = realloc(text_buf, off + tlen + 1);
                        if (tmp) {
                            text_buf = tmp;
                            memcpy(text_buf + off, text->valuestring, tlen);
                            off += tlen;
                            text_buf[off] = '\0';
                        }
                    }
                } else if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "tool_use") == 0) {
                    if (!tool_calls) tool_calls = cJSON_CreateArray();
                    cJSON *id = cJSON_GetObjectItem(block, "id");
                    cJSON *name = cJSON_GetObjectItem(block, "name");
                    cJSON *input = cJSON_GetObjectItem(block, "input");
                    if (!name || !cJSON_IsString(name)) continue;

                    cJSON *tc = cJSON_CreateObject();
                    if (id && cJSON_IsString(id)) {
                        cJSON_AddStringToObject(tc, "id", id->valuestring);
                    }
                    cJSON_AddStringToObject(tc, "type", "function");
                    cJSON *func = cJSON_CreateObject();
                    cJSON_AddStringToObject(func, "name", name->valuestring);
                    if (input) {
                        char *args = cJSON_PrintUnformatted(input);
                        if (args) {
                            cJSON_AddStringToObject(func, "arguments", args);
                            free(args);
                        }
                    }
                    cJSON_AddItemToObject(tc, "function", func);
                    cJSON_AddItemToArray(tool_calls, tc);
                }
            }
            if (text_buf) {
                cJSON_AddStringToObject(m, "content", text_buf);
            } else {
                cJSON_AddStringToObject(m, "content", "");
            }
            if (tool_calls) {
                cJSON_AddItemToObject(m, "tool_calls", tool_calls);
            }
            cJSON_AddItemToArray(out, m);
            free(text_buf);
        } else if (strcmp(role->valuestring, "user") == 0) {
            /* tool_result blocks become role=tool */
            cJSON *block;
            bool has_user_text = false;
            char *text_buf = NULL;
            size_t off = 0;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "tool_result") == 0) {
                    cJSON *tool_id = cJSON_GetObjectItem(block, "tool_use_id");
                    cJSON *tcontent = cJSON_GetObjectItem(block, "content");
                    if (!tool_id || !cJSON_IsString(tool_id)) continue;
                    cJSON *tm = cJSON_CreateObject();
                    cJSON_AddStringToObject(tm, "role", "tool");
                    cJSON_AddStringToObject(tm, "tool_call_id", tool_id->valuestring);
                    if (tcontent && cJSON_IsString(tcontent)) {
                        cJSON_AddStringToObject(tm, "content", tcontent->valuestring);
                    } else {
                        cJSON_AddStringToObject(tm, "content", "");
                    }
                    cJSON_AddItemToArray(out, tm);
                } else if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        size_t tlen = strlen(text->valuestring);
                        char *tmp = realloc(text_buf, off + tlen + 1);
                        if (tmp) {
                            text_buf = tmp;
                            memcpy(text_buf + off, text->valuestring, tlen);
                            off += tlen;
                            text_buf[off] = '\0';
                        }
                        has_user_text = true;
                    }
                }
            }
            if (has_user_text) {
                cJSON *um = cJSON_CreateObject();
                cJSON_AddStringToObject(um, "role", "user");
                cJSON_AddStringToObject(um, "content", text_buf);
                cJSON_AddItemToArray(out, um);
            }
            free(text_buf);
        }
    }

    return out;
}

/* ── Public: chat with tools (non-streaming) ──────────────────── */

void llm_response_free(llm_response_t *resp)
{
    free(resp->text);
    resp->text = NULL;
    resp->text_len = 0;
    for (int i = 0; i < resp->call_count; i++) {
        free(resp->calls[i].input);
        resp->calls[i].input = NULL;
    }
    resp->call_count = 0;
    resp->tool_use = false;
}

esp_err_t llm_chat_tools(const char *system_prompt,
                         cJSON *messages,
                         const char *tools_json,
                         llm_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));

    if (s_api_key[0] == '\0') return ESP_ERR_INVALID_STATE;

    /* Build request body (non-streaming) */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", s_model_id);
    if (strncasecmp(s_model_id, "gpt-5", 5) == 0 || strncasecmp(s_model_id, "o1", 2) == 0) {
        cJSON_AddNumberToObject(body, "max_completion_tokens", MIMI_LLM_MAX_TOKENS);
    } else {
        cJSON_AddNumberToObject(body, "max_tokens", MIMI_LLM_MAX_TOKENS);
    }

    if (llm_protocol_is_openai()) {
        cJSON *openai_msgs = convert_messages_openai(system_prompt, messages);
        cJSON_AddItemToObject(body, "messages", openai_msgs);

        if (tools_json) {
            cJSON *tools = convert_tools_openai(tools_json);
            if (tools) {
                cJSON_AddItemToObject(body, "tools", tools);
                cJSON_AddStringToObject(body, "tool_choice", "auto");
            }
        }
    } else {
        cJSON_AddStringToObject(body, "system", system_prompt);

        /* Deep-copy messages so caller keeps ownership */
        cJSON *msgs_copy = cJSON_Duplicate(messages, 1);
        cJSON_AddItemToObject(body, "messages", msgs_copy);

        /* Add tools array if provided */
        if (tools_json) {
            cJSON *tools = cJSON_Parse(tools_json);
            if (tools) {
                cJSON_AddItemToObject(body, "tools", tools);
            }
        }
    }

    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Calling LLM API with tools (protocol: %s, model: %s, body: %d bytes)",
             llm_protocol_name(s_protocol), s_model_id, (int)strlen(post_data));
    llm_log_payload("LLM tools request", post_data);

    /* HTTP call */
    resp_buf_t rb;
    if (resp_buf_init(&rb, MIMI_LLM_STREAM_BUF_SIZE) != ESP_OK) {
        free(post_data);
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = llm_http_call(post_data, &rb, &status);
    free(post_data);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        llm_log_payload("LLM tools partial response", rb.data);
        resp_buf_free(&rb);
        return err;
    }

    llm_log_payload("LLM tools raw response", rb.data);

    if (status != 200) {
        ESP_LOGE(TAG, "API error %d: %.500s", status, rb.data ? rb.data : "");
        resp_buf_free(&rb);
        return ESP_FAIL;
    }

    /* Parse full JSON response */
    cJSON *root = cJSON_Parse(rb.data);
    resp_buf_free(&rb);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse API response JSON");
        return ESP_FAIL;
    }

    if (llm_protocol_is_openai()) {
        cJSON *choices = cJSON_GetObjectItem(root, "choices");
        cJSON *choice0 = choices && cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        if (choice0) {
            cJSON *finish = cJSON_GetObjectItem(choice0, "finish_reason");
            if (finish && cJSON_IsString(finish)) {
                resp->tool_use = (strcmp(finish->valuestring, "tool_calls") == 0);
            }

            cJSON *message = cJSON_GetObjectItem(choice0, "message");
            if (message) {
                cJSON *content = cJSON_GetObjectItem(message, "content");
                if (content && cJSON_IsString(content)) {
                    size_t tlen = strlen(content->valuestring);
                    resp->text = calloc(1, tlen + 1);
                    if (resp->text) {
                        memcpy(resp->text, content->valuestring, tlen);
                        resp->text_len = tlen;
                    }
                }

                cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
                if (tool_calls && cJSON_IsArray(tool_calls)) {
                    cJSON *tc;
                    cJSON_ArrayForEach(tc, tool_calls) {
                        if (resp->call_count >= MIMI_MAX_TOOL_CALLS) break;
                        llm_tool_call_t *call = &resp->calls[resp->call_count];
                        cJSON *id = cJSON_GetObjectItem(tc, "id");
                        cJSON *func = cJSON_GetObjectItem(tc, "function");
                        if (id && cJSON_IsString(id)) {
                            strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
                        }
                        if (func) {
                            cJSON *name = cJSON_GetObjectItem(func, "name");
                            cJSON *args = cJSON_GetObjectItem(func, "arguments");
                            if (name && cJSON_IsString(name)) {
                                strncpy(call->name, name->valuestring, sizeof(call->name) - 1);
                            }
                            if (args && cJSON_IsString(args)) {
                                call->input = strdup(args->valuestring);
                                if (call->input) {
                                    call->input_len = strlen(call->input);
                                }
                            }
                        }
                        resp->call_count++;
                    }
                    if (resp->call_count > 0) {
                        resp->tool_use = true;
                    }
                }
            }
        }
    } else {
        /* stop_reason */
        cJSON *stop_reason = cJSON_GetObjectItem(root, "stop_reason");
        if (stop_reason && cJSON_IsString(stop_reason)) {
            resp->tool_use = (strcmp(stop_reason->valuestring, "tool_use") == 0);
        }

        /* Iterate content blocks */
        cJSON *content = cJSON_GetObjectItem(root, "content");
        if (content && cJSON_IsArray(content)) {
            /* Accumulate total text length first */
            size_t total_text = 0;
            cJSON *block;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (btype && strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        total_text += strlen(text->valuestring);
                    }
                }
            }

            /* Allocate and copy text */
            if (total_text > 0) {
                resp->text = calloc(1, total_text + 1);
                if (resp->text) {
                    cJSON_ArrayForEach(block, content) {
                        cJSON *btype = cJSON_GetObjectItem(block, "type");
                        if (!btype || strcmp(btype->valuestring, "text") != 0) continue;
                        cJSON *text = cJSON_GetObjectItem(block, "text");
                        if (!text || !cJSON_IsString(text)) continue;
                        size_t tlen = strlen(text->valuestring);
                        memcpy(resp->text + resp->text_len, text->valuestring, tlen);
                        resp->text_len += tlen;
                    }
                    resp->text[resp->text_len] = '\0';
                }
            }

            /* Extract tool_use blocks */
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (!btype || strcmp(btype->valuestring, "tool_use") != 0) continue;
                if (resp->call_count >= MIMI_MAX_TOOL_CALLS) break;

                llm_tool_call_t *call = &resp->calls[resp->call_count];

                cJSON *id = cJSON_GetObjectItem(block, "id");
                if (id && cJSON_IsString(id)) {
                    strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
                }

                cJSON *name = cJSON_GetObjectItem(block, "name");
                if (name && cJSON_IsString(name)) {
                    strncpy(call->name, name->valuestring, sizeof(call->name) - 1);
                }

                cJSON *input = cJSON_GetObjectItem(block, "input");
                if (input) {
                    char *input_str = cJSON_PrintUnformatted(input);
                    if (input_str) {
                        call->input = input_str;
                        call->input_len = strlen(input_str);
                    }
                }

                resp->call_count++;
            }
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Response: %d bytes text, %d tool calls, stop=%s",
             (int)resp->text_len, resp->call_count,
             resp->tool_use ? "tool_use" : "end_turn");

    return ESP_OK;
}

/* ── NVS helpers ──────────────────────────────────────────────── */

esp_err_t llm_set_api_key(const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    safe_copy(s_api_key, sizeof(s_api_key), api_key);
    ESP_LOGI(TAG, "API key saved");
    return ESP_OK;
}

esp_err_t llm_set_api_base(const char *api_base)
{
    /* Validate before persisting - use validation-only function */
    esp_err_t err = llm_validate_api_base(api_base);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Invalid API base format: %s", api_base ? api_base : "<null>");
        return err;
    }

    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_BASE, api_base));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    safe_copy(s_api_base, sizeof(s_api_base), api_base);
    llm_recompute_effective_config();
    ESP_LOGI(TAG, "API base set");
    return ESP_OK;
}

esp_err_t llm_set_model(const char *model)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_MODEL, model));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    safe_copy(s_model, sizeof(s_model), model);
    llm_recompute_effective_config();
    ESP_LOGI(TAG, "Model set to: %s", s_model);
    return ESP_OK;
}

esp_err_t llm_set_provider(const char *provider)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_PROVIDER, provider));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    safe_copy(s_provider, sizeof(s_provider), provider);
    llm_recompute_effective_config();
    ESP_LOGI(TAG, "Provider set to: %s", s_provider);
    return ESP_OK;
}
