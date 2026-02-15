#include "feishu_bot.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "feishu";

/* Feishu API endpoints */
#define FEISHU_API_BASE         "https://open.feishu.cn/open-apis"
#define FEISHU_AUTH_URL         FEISHU_API_BASE "/auth/v3/tenant_access_token/internal"
#define FEISHU_MSG_URL          FEISHU_API_BASE "/im/v1/messages"

/* Webhook limits */
#define FEISHU_WEBHOOK_MAX_BODY 4096
#define FEISHU_API_MAX_RETRIES  2

/* Standard ACK response for Feishu event callback */
#define FEISHU_EVENT_ACK_RESPONSE  "{\"code\":0,\"msg\":\"success\"}"

static char s_app_id[64] = MIMI_SECRET_FEISHU_APP_ID;
static char s_app_secret[128] = MIMI_SECRET_FEISHU_APP_SECRET;
static char s_tenant_token[512] = {0};
static int64_t s_token_expire_time = 0;

/* HTTP response accumulator */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp->len + evt->data_len >= resp->cap) {
            size_t new_cap = resp->cap * 2;
            if (new_cap < resp->len + evt->data_len + 1) {
                new_cap = resp->len + evt->data_len + 1;
            }
            char *tmp = realloc(resp->buf, new_cap);
            if (!tmp) return ESP_ERR_NO_MEM;
            resp->buf = tmp;
            resp->cap = new_cap;
        }
        memcpy(resp->buf + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
        resp->buf[resp->len] = '\0';
    }
    return ESP_OK;
}

/* ── Get tenant access token ────────────────────────────── */
static esp_err_t feishu_get_tenant_token(void)
{
    if (s_app_id[0] == '\0' || s_app_secret[0] == '\0') {
        ESP_LOGW(TAG, "No Feishu credentials configured");
        return ESP_ERR_INVALID_STATE;
    }

    /* Check if token is still valid (with 5 min buffer) */
    int64_t now = esp_timer_get_time() / 1000000LL;
    if (s_tenant_token[0] != '\0' && s_token_expire_time > now + 300) {
        return ESP_OK;
    }

    /* Build request body */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "app_id", s_app_id);
    cJSON_AddStringToObject(body, "app_secret", s_app_secret);
    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    http_resp_t resp = {
        .buf = calloc(1, 2048),
        .len = 0,
        .cap = 2048,
    };
    if (!resp.buf) {
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = FEISHU_AUTH_URL,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(json_str);
        free(resp.buf);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    free(json_str);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return err;
    }

    /* Parse response */
    cJSON *root = cJSON_Parse(resp.buf);
    free(resp.buf);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse token response");
        return ESP_FAIL;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!code || code->valueint != 0) {
        ESP_LOGE(TAG, "Token request failed: code=%d", code ? code->valueint : -1);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *token = cJSON_GetObjectItem(root, "tenant_access_token");
    cJSON *expire = cJSON_GetObjectItem(root, "expire");
    
    if (token && cJSON_IsString(token)) {
        strncpy(s_tenant_token, token->valuestring, sizeof(s_tenant_token) - 1);
        s_token_expire_time = now + (expire ? expire->valueint : 7200) - 300;
        ESP_LOGI(TAG, "Got tenant access token, expires in %d seconds", 
                 expire ? expire->valueint : 7200);
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/* ── Feishu API call with retry ─────────────────────────── */
static char *feishu_api_call(const char *url, const char *method, const char *post_data)
{
    /* Ensure we have a valid token */
    if (feishu_get_tenant_token() != ESP_OK) {
        return NULL;
    }

    for (int attempt = 0; attempt <= FEISHU_API_MAX_RETRIES; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(500 * attempt));
            ESP_LOGW(TAG, "API retry %d/%d", attempt, FEISHU_API_MAX_RETRIES);
        }

        http_resp_t resp = {
            .buf = calloc(1, 4096),
            .len = 0,
            .cap = 4096,
        };
        if (!resp.buf) return NULL;

        esp_http_client_config_t config = {
            .url = url,
            .event_handler = http_event_handler,
            .user_data = &resp,
            .timeout_ms = 15000,
            .buffer_size = 2048,
            .buffer_size_tx = 2048,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            free(resp.buf);
            continue;
        }

        char auth_header[600];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_tenant_token);
        esp_http_client_set_header(client, "Authorization", auth_header);
        esp_http_client_set_header(client, "Content-Type", "application/json");

        if (strcmp(method, "POST") == 0) {
            esp_http_client_set_method(client, HTTP_METHOD_POST);
            if (post_data) {
                esp_http_client_set_post_field(client, post_data, strlen(post_data));
            }
        } else {
            esp_http_client_set_method(client, HTTP_METHOD_GET);
        }

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err != ESP_OK) {
            /* Network/timeout errors are transient — use WARN level */
            ESP_LOGW(TAG, "HTTP request failed: %s", esp_err_to_name(err));
            free(resp.buf);
            continue;
        }

        /* Success: 2xx */
        if (status >= 200 && status < 300) {
            return resp.buf;
        }

        /* Client errors (4xx): not retryable */
        if (status >= 400 && status < 500) {
            ESP_LOGE(TAG, "API client error: HTTP %d", status);
            free(resp.buf);
            return NULL;
        }

        /* Server errors (5xx) or unexpected status: retryable */
        ESP_LOGW(TAG, "API error: HTTP %d, retrying", status);
        free(resp.buf);
    }

    ESP_LOGE(TAG, "API call failed after %d retries", FEISHU_API_MAX_RETRIES);
    return NULL;
}

/* ── Parse and dispatch incoming Feishu message ─────────── */
static void handle_message_event(const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse request JSON");
        return;
    }

    /* Navigate: root -> event -> message -> {chat_id, content} */
    cJSON *event = cJSON_GetObjectItem(root, "event");
    cJSON *message = event ? cJSON_GetObjectItem(event, "message") : NULL;
    if (!message) {
        ESP_LOGE(TAG, "Missing event.message in payload");
        cJSON_Delete(root);
        return;
    }

    cJSON *chat_id = cJSON_GetObjectItem(message, "chat_id");
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (!chat_id || !cJSON_IsString(chat_id) || !content || !cJSON_IsString(content)) {
        ESP_LOGE(TAG, "Missing chat_id or content in message");
        cJSON_Delete(root);
        return;
    }

    /* Parse nested content JSON: {"text": "..."} */
    cJSON *msg_content = cJSON_Parse(content->valuestring);
    if (!msg_content) {
        ESP_LOGE(TAG, "Failed to parse message content JSON");
        cJSON_Delete(root);
        return;
    }

    cJSON *text = cJSON_GetObjectItem(msg_content, "text");
    if (text && cJSON_IsString(text)) {
        mimi_msg_t msg = {0};
        strncpy(msg.channel, MIMI_CHAN_FEISHU, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, chat_id->valuestring, sizeof(msg.chat_id) - 1);
        msg.content = strdup(text->valuestring);

        if (msg.content) {
            ESP_LOGI(TAG, "Message from Feishu chat %s: %.50s...",
                     chat_id->valuestring, text->valuestring);
            esp_err_t ret = message_bus_push_inbound(&msg);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to push message to bus: %s", esp_err_to_name(ret));
                free(msg.content);
            }
        }
    }

    cJSON_Delete(msg_content);
    cJSON_Delete(root);
}

/* ── Webhook HTTP handler ───────────────────────────────── */
static httpd_handle_t s_webhook_server = NULL;

static esp_err_t feishu_webhook_handler(httpd_req_t *req)
{
    /* Only accept POST requests */
    if (req->method != HTTP_POST) {
        httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method Not Allowed");
        return ESP_OK;
    }

    /* Reject oversized payloads to mitigate cJSON stack overflow risk */
    size_t content_len = req->content_len;
    if (content_len == 0 || content_len > FEISHU_WEBHOOK_MAX_BODY) {
        ESP_LOGW(TAG, "Request body rejected: size=%d", (int)content_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Content Length");
        return ESP_OK;
    }

    /* Allocate buffer on heap instead of stack to avoid stack overflow */
    char *buffer = malloc(content_len + 1);
    if (!buffer) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes for request body", (int)content_len);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of Memory");
        return ESP_OK;
    }

    int len = httpd_req_recv(req, buffer, content_len);
    if (len <= 0) {
        ESP_LOGE(TAG, "Failed to read request body");
        free(buffer);
        httpd_resp_send(req, FEISHU_EVENT_ACK_RESPONSE, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    buffer[len] = '\0';

    ESP_LOGD(TAG, "Received request: len=%d", len);

    /* URL verification (challenge) */
    if (strstr(buffer, "challenge") != NULL) {
        ESP_LOGI(TAG, "URL verification challenge handled");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, buffer, HTTPD_RESP_USE_STRLEN);
        free(buffer);
        return ESP_OK;
    }

    /* Message event */
    if (strstr(buffer, "im.message.receive_v1") != NULL) {
        handle_message_event(buffer);
    } else {
        ESP_LOGW(TAG, "Unknown request type received");
    }

    free(buffer);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, FEISHU_EVENT_ACK_RESPONSE, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t feishu_webhook_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8765;
    config.max_open_sockets = 4;
    config.stack_size = 8192;
    
    ESP_LOGI(TAG, "Starting Feishu webhook server on port %d", config.server_port);
    
    esp_err_t ret = httpd_start(&s_webhook_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Feishu webhook server: %s", esp_err_to_name(ret));
        return ret;
    }
    
    httpd_uri_t webhook_uri = {
        .uri = "/feishu/webhook",
        .method = HTTP_POST,
        .handler = feishu_webhook_handler,
        .is_websocket = false,
    };
    
    ret = httpd_register_uri_handler(s_webhook_server, &webhook_uri);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register URI handler: %s", esp_err_to_name(ret));
        httpd_stop(s_webhook_server);
        s_webhook_server = NULL;
        return ret;
    }
    
    ESP_LOGI(TAG, "Feishu webhook server started on port %d", config.server_port);
    return ESP_OK;
}

static esp_err_t feishu_webhook_server_stop(void)
{
    if (s_webhook_server) {
        httpd_stop(s_webhook_server);
        s_webhook_server = NULL;
        ESP_LOGI(TAG, "Feishu webhook server stopped");
    }
    return ESP_OK;
}

/* ── Determine receive_id_type from chat_id prefix ──────── */
static const char *get_receive_id_type(const char *chat_id)
{
    if (strncmp(chat_id, "ou_", 3) == 0) return "open_id";
    if (strncmp(chat_id, "on_", 3) == 0) return "union_id";
    if (strncmp(chat_id, "oc_", 3) == 0) return "chat_id";
    return "chat_id";
}

/**
 * Adjust a split position to avoid breaking UTF-8 multi-byte characters.
 * If pos falls on a continuation byte (10xxxxxx), back up to the leading byte.
 */
static size_t utf8_safe_split(const char *text, size_t pos)
{
    /* Walk back while the byte at pos is a UTF-8 continuation byte (0x80..0xBF) */
    while (pos > 0 && ((uint8_t)text[pos] & 0xC0) == 0x80) {
        pos--;
    }
    return pos;
}

/**
 * Build the Feishu text message JSON body for a given segment.
 * Returns a heap-allocated JSON string, or NULL on failure.
 * Caller must free the returned string.
 */
static char *build_text_message_json(const char *chat_id,
                                     const char *text, size_t offset, size_t chunk)
{
    /* Build nested content: {"text": "..."} */
    cJSON *content_obj = cJSON_CreateObject();
    if (!content_obj) return NULL;

    /* Copy the segment to a temporary buffer for null-termination */
    char *segment = malloc(chunk + 1);
    if (!segment) { cJSON_Delete(content_obj); return NULL; }
    memcpy(segment, text + offset, chunk);
    segment[chunk] = '\0';
    cJSON_AddStringToObject(content_obj, "text", segment);
    free(segment);

    char *content_str = cJSON_PrintUnformatted(content_obj);
    cJSON_Delete(content_obj);
    if (!content_str) return NULL;

    /* Build outer message body */
    cJSON *body = cJSON_CreateObject();
    if (!body) { free(content_str); return NULL; }

    cJSON_AddStringToObject(body, "receive_id", chat_id);
    cJSON_AddStringToObject(body, "msg_type", "text");
    cJSON_AddStringToObject(body, "content", content_str);
    free(content_str);

    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return json_str;  /* May be NULL on OOM */
}

/* ── Public API ─────────────────────────────────────────── */

esp_err_t feishu_bot_init(void)
{
    /* NVS overrides take highest priority */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_FEISHU, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp_id[64] = {0};
        char tmp_secret[128] = {0};
        size_t len_id = sizeof(tmp_id);
        size_t len_secret = sizeof(tmp_secret);
        
        if (nvs_get_str(nvs, MIMI_NVS_KEY_FEISHU_APP_ID, tmp_id, &len_id) == ESP_OK && tmp_id[0]) {
            strncpy(s_app_id, tmp_id, sizeof(s_app_id) - 1);
        }
        if (nvs_get_str(nvs, MIMI_NVS_KEY_FEISHU_APP_SECRET, tmp_secret, &len_secret) == ESP_OK && tmp_secret[0]) {
            strncpy(s_app_secret, tmp_secret, sizeof(s_app_secret) - 1);
        }
        nvs_close(nvs);
    }

    if (s_app_id[0] && s_app_secret[0]) {
        ESP_LOGI(TAG, "Feishu credentials loaded (app_id=%s)", s_app_id);
    } else {
        ESP_LOGW(TAG, "No Feishu credentials. Use CLI: set_feishu_creds <APP_ID> <APP_SECRET>");
    }
    
    return ESP_OK;
}

esp_err_t feishu_bot_stop(void)
{
    return feishu_webhook_server_stop();
}

esp_err_t feishu_bot_start(void)
{
    esp_err_t ret = feishu_webhook_server_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Feishu webhook server: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t feishu_send_message(const char *chat_id, const char *text)
{
    if (s_app_id[0] == '\0' || s_app_secret[0] == '\0') {
        ESP_LOGW(TAG, "Cannot send: no credentials configured");
        return ESP_ERR_INVALID_STATE;
    }

    if (!chat_id || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s?receive_id_type=%s",
             FEISHU_MSG_URL, get_receive_id_type(chat_id));

    size_t text_len = strlen(text);
    size_t offset = 0;
    esp_err_t result = ESP_OK;

    while (offset < text_len) {
        size_t chunk = text_len - offset;
        if (chunk > MIMI_FEISHU_MAX_MSG_LEN) {
            chunk = MIMI_FEISHU_MAX_MSG_LEN;
            /* Avoid splitting in the middle of a UTF-8 multi-byte character */
            chunk = utf8_safe_split(text + offset, chunk);
            if (chunk == 0) chunk = MIMI_FEISHU_MAX_MSG_LEN; /* Safety: avoid infinite loop */
        }

        /* Build JSON via helper (handles all internal cleanup) */
        char *json_str = build_text_message_json(chat_id, text, offset, chunk);
        if (!json_str) {
            ESP_LOGE(TAG, "Failed to build message JSON");
            result = ESP_ERR_NO_MEM;
            break;
        }

        char *resp = feishu_api_call(url, "POST", json_str);
        free(json_str);

        if (resp) {
            /* Check Feishu business-level error code */
            cJSON *root = cJSON_Parse(resp);
            if (root) {
                cJSON *code = cJSON_GetObjectItem(root, "code");
                cJSON *msg = cJSON_GetObjectItem(root, "msg");
                if (!code || code->valueint != 0) {
                    ESP_LOGE(TAG, "Send failed: code=%d, msg=%s",
                            code ? code->valueint : -1,
                            msg ? msg->valuestring : "unknown");
                }
                cJSON_Delete(root);
            }
            free(resp);
        } else {
            ESP_LOGE(TAG, "API call failed for message chunk");
            result = ESP_FAIL;
            /* Continue sending remaining chunks despite partial failure */
        }

        offset += chunk;
    }

    return result;
}

esp_err_t feishu_set_credentials(const char *app_id, const char *app_secret)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_FEISHU, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_FEISHU_APP_ID, app_id));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_FEISHU_APP_SECRET, app_secret));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_app_id, app_id, sizeof(s_app_id) - 1);
    strncpy(s_app_secret, app_secret, sizeof(s_app_secret) - 1);
    
    /* Clear cached token */
    s_tenant_token[0] = '\0';
    s_token_expire_time = 0;
    
    ESP_LOGI(TAG, "Feishu credentials saved");
    return ESP_OK;
}
