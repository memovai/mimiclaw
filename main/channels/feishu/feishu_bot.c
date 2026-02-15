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
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "feishu";

/* Feishu API endpoints */
#define FEISHU_API_BASE         "https://open.feishu.cn/open-apis"
#define FEISHU_AUTH_URL         FEISHU_API_BASE "/auth/v3/tenant_access_token/internal"
#define FEISHU_EVENT_URL        FEISHU_API_BASE "/im/v1/messages"

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

/* ── Feishu API call (direct path) ──────────────────────── */
static char *feishu_api_call(const char *url, const char *method, const char *post_data)
{
    /* Ensure we have a valid token */
    if (feishu_get_tenant_token() != ESP_OK) {
        return NULL;
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
        return NULL;
    }

    /* Set headers */
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
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return NULL;
    }

    return resp.buf;
}

/* ── Message polling (simulated - in real scenario use event subscription) ── */
/* Note: Feishu uses event callback mode, not polling like Telegram.
 * For simplicity, we'll implement a basic message sending capability only.
 * Full implementation would require webhook server or websocket connection.
 */

static void feishu_poll_task(void *arg)
{
    ESP_LOGI(TAG, "Feishu polling task started");
    ESP_LOGW(TAG, "Note: Feishu uses event subscription, not polling.");
    ESP_LOGW(TAG, "This task is a placeholder. Configure event callback URL in Feishu Admin.");

    while (1) {
        /* In a real implementation, this would be replaced by webhook event handling */
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
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

esp_err_t feishu_bot_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        feishu_poll_task, "feishu_poll",
        MIMI_FEISHU_POLL_STACK, NULL,
        MIMI_FEISHU_POLL_PRIO, NULL, MIMI_FEISHU_POLL_CORE);

    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t feishu_send_message(const char *chat_id, const char *text)
{
    if (s_app_id[0] == '\0' || s_app_secret[0] == '\0') {
        ESP_LOGW(TAG, "Cannot send: no credentials configured");
        return ESP_ERR_INVALID_STATE;
    }

    /* Build message URL */
    char url[256];
    snprintf(url, sizeof(url), "%s?receive_id_type=chat_id", FEISHU_EVENT_URL);

    /* Split long messages at 4096-char boundary */
    size_t text_len = strlen(text);
    size_t offset = 0;

    while (offset < text_len) {
        size_t chunk = text_len - offset;
        if (chunk > MIMI_FEISHU_MAX_MSG_LEN) {
            chunk = MIMI_FEISHU_MAX_MSG_LEN;
        }

        /* Create message segment */
        char *segment = malloc(chunk + 1);
        if (!segment) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(segment, text + offset, chunk);
        segment[chunk] = '\0';

        /* Build JSON body */
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "receive_id", chat_id);
        cJSON_AddStringToObject(body, "msg_type", "text");
        
        cJSON *content = cJSON_CreateObject();
        cJSON_AddStringToObject(content, "text", segment);
        char *content_str = cJSON_PrintUnformatted(content);
        cJSON_Delete(content);
        
        if (content_str) {
            cJSON_AddStringToObject(body, "content", content_str);
            free(content_str);
        }

        char *json_str = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        free(segment);

        if (json_str) {
            char *resp = feishu_api_call(url, "POST", json_str);
            free(json_str);

            if (resp) {
                /* Check response */
                cJSON *root = cJSON_Parse(resp);
                if (root) {
                    cJSON *code = cJSON_GetObjectItem(root, "code");
                    if (code && code->valueint != 0) {
                        cJSON *msg = cJSON_GetObjectItem(root, "msg");
                        ESP_LOGW(TAG, "Send message failed: code=%d, msg=%s", 
                                code->valueint, msg ? msg->valuestring : "unknown");
                    }
                    cJSON_Delete(root);
                }
                free(resp);
            } else {
                ESP_LOGE(TAG, "Failed to send message chunk");
            }
        }

        offset += chunk;
    }

    return ESP_OK;
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
