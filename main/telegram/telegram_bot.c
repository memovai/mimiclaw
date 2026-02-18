#include "telegram_bot.h"
#include "telegram/md_to_html.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "telegram";

static char s_bot_token[128] = MIMI_SECRET_TG_TOKEN;
static int64_t s_update_offset = 0;

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

/* ── Proxy path: manual HTTP over CONNECT tunnel ────────────── */

static char *tg_api_call_via_proxy(const char *path, const char *post_data)
{
    proxy_conn_t *conn = proxy_conn_open("api.telegram.org", 443,
                                          (MIMI_TG_POLL_TIMEOUT_S + 5) * 1000);
    if (!conn) return NULL;

    /* Build HTTP request */
    char header[512];
    int hlen;
    if (post_data) {
        hlen = snprintf(header, sizeof(header),
            "POST /bot%s/%s HTTP/1.1\r\n"
            "Host: api.telegram.org\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            s_bot_token, path, (int)strlen(post_data));
    } else {
        hlen = snprintf(header, sizeof(header),
            "GET /bot%s/%s HTTP/1.1\r\n"
            "Host: api.telegram.org\r\n"
            "Connection: close\r\n\r\n",
            s_bot_token, path);
    }

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return NULL;
    }
    if (post_data && proxy_conn_write(conn, post_data, strlen(post_data)) < 0) {
        proxy_conn_close(conn);
        return NULL;
    }

    /* Read response — accumulate until connection close */
    size_t cap = 4096, len = 0;
    char *buf = calloc(1, cap);
    if (!buf) { proxy_conn_close(conn); return NULL; }

    int timeout = (MIMI_TG_POLL_TIMEOUT_S + 5) * 1000;
    while (1) {
        if (len + 1024 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) break;
            buf = tmp;
        }
        int n = proxy_conn_read(conn, buf + len, cap - len - 1, timeout);
        if (n <= 0) break;
        len += n;
    }
    buf[len] = '\0';
    proxy_conn_close(conn);

    /* Skip HTTP headers — find \r\n\r\n */
    char *body = strstr(buf, "\r\n\r\n");
    if (!body) { free(buf); return NULL; }
    body += 4;

    /* Return just the body */
    char *result = strdup(body);
    free(buf);
    return result;
}

/* ── Direct path: esp_http_client ───────────────────────────── */

static char *tg_api_call_direct(const char *method, const char *post_data)
{
    char url[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/%s", s_bot_token, method);

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
        .timeout_ms = (MIMI_TG_POLL_TIMEOUT_S + 5) * 1000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        return NULL;
    }

    if (post_data) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
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

static char *tg_api_call(const char *method, const char *post_data)
{
    if (http_proxy_is_enabled()) {
        return tg_api_call_via_proxy(method, post_data);
    }
    return tg_api_call_direct(method, post_data);
}

static void process_updates(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) return;

    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    if (!cJSON_IsTrue(ok)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsArray(result)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *update;
    cJSON_ArrayForEach(update, result) {
        /* Track offset */
        cJSON *update_id = cJSON_GetObjectItem(update, "update_id");
        if (cJSON_IsNumber(update_id)) {
            int64_t uid = (int64_t)update_id->valuedouble;
            if (uid >= s_update_offset) {
                s_update_offset = uid + 1;
            }
        }

        /* Extract message */
        cJSON *message = cJSON_GetObjectItem(update, "message");
        if (!message) continue;

        cJSON *text = cJSON_GetObjectItem(message, "text");
        if (!text || !cJSON_IsString(text)) continue;

        cJSON *chat = cJSON_GetObjectItem(message, "chat");
        if (!chat) continue;

        cJSON *chat_id = cJSON_GetObjectItem(chat, "id");
        if (!chat_id) continue;

        char chat_id_str[32];
        snprintf(chat_id_str, sizeof(chat_id_str), "%.0f", chat_id->valuedouble);

        ESP_LOGI(TAG, "Message from chat %s: %.40s...", chat_id_str, text->valuestring);

        /* Push to inbound bus */
        mimi_msg_t msg = {0};
        strncpy(msg.channel, MIMI_CHAN_TELEGRAM, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, chat_id_str, sizeof(msg.chat_id) - 1);
        msg.content = strdup(text->valuestring);
        if (msg.content) {
            message_bus_push_inbound(&msg);
        }
    }

    cJSON_Delete(root);
}

static void telegram_poll_task(void *arg)
{
    ESP_LOGI(TAG, "Telegram polling task started");

    while (1) {
        if (s_bot_token[0] == '\0') {
            ESP_LOGW(TAG, "No bot token configured, waiting...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        char params[128];
        snprintf(params, sizeof(params),
                 "getUpdates?offset=%" PRId64 "&timeout=%d",
                 s_update_offset, MIMI_TG_POLL_TIMEOUT_S);

        char *resp = tg_api_call(params, NULL);
        if (resp) {
            process_updates(resp);
            free(resp);
        } else {
            /* Back off on error */
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}

/* --- Public API --- */

esp_err_t telegram_bot_init(void)
{
    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_TG, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_TG_TOKEN, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_bot_token, tmp, sizeof(s_bot_token) - 1);
        }
        nvs_close(nvs);
    }

    /* s_bot_token is already initialized from MIMI_SECRET_TG_TOKEN as fallback */

    if (s_bot_token[0]) {
        ESP_LOGI(TAG, "Telegram bot token loaded (len=%d)", (int)strlen(s_bot_token));
    } else {
        ESP_LOGW(TAG, "No Telegram bot token. Use CLI: set_tg_token <TOKEN>");
    }
    return ESP_OK;
}

esp_err_t telegram_bot_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        telegram_poll_task, "tg_poll",
        MIMI_TG_POLL_STACK, NULL,
        MIMI_TG_POLL_PRIO, NULL, MIMI_TG_POLL_CORE);

    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t telegram_send_message(const char *chat_id, const char *text)
{
    if (s_bot_token[0] == '\0') {
        ESP_LOGW(TAG, "Cannot send: no bot token");
        return ESP_ERR_INVALID_STATE;
    }

    /* Split long messages at 4096-char boundary */
    size_t text_len = strlen(text);
    size_t offset = 0;

    while (offset < text_len) {
        size_t chunk = text_len - offset;
        if (chunk > MIMI_TG_MAX_MSG_LEN) {
            chunk = MIMI_TG_MAX_MSG_LEN;
        }

        /* Create null-terminated chunk */
        char *segment = malloc(chunk + 1);
        if (!segment) return ESP_ERR_NO_MEM;
        memcpy(segment, text + offset, chunk);
        segment[chunk] = '\0';

        /* Convert Markdown to Telegram HTML */
        size_t html_cap = chunk * 3 + 256;  /* worst case: every char escaped */
        char *html_buf = malloc(html_cap);
        if (!html_buf) {
            free(segment);
            return ESP_ERR_NO_MEM;
        }
        md_to_telegram_html(segment, html_buf, html_cap);
        free(segment);

        /* Build JSON body with HTML parse_mode */
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "chat_id", chat_id);
        cJSON_AddStringToObject(body, "text", html_buf);
        cJSON_AddStringToObject(body, "parse_mode", "HTML");
        free(html_buf);

        char *json_str = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);

        if (json_str) {
            char *resp = tg_api_call("sendMessage", json_str);
            free(json_str);
            if (resp) {
                /* Check for HTML parse error, retry as plain text */
                cJSON *root = cJSON_Parse(resp);
                if (root) {
                    cJSON *ok_field = cJSON_GetObjectItem(root, "ok");
                    if (!cJSON_IsTrue(ok_field)) {
                        ESP_LOGW(TAG, "HTML send failed, retrying plain text");
                        cJSON_Delete(root);
                        free(resp);

                        /* Retry without parse_mode */
                        cJSON *body2 = cJSON_CreateObject();
                        cJSON_AddStringToObject(body2, "chat_id", chat_id);
                        char *seg2 = malloc(chunk + 1);
                        if (seg2) {
                            memcpy(seg2, text + offset, chunk);
                            seg2[chunk] = '\0';
                            cJSON_AddStringToObject(body2, "text", seg2);
                            free(seg2);
                        }
                        char *json2 = cJSON_PrintUnformatted(body2);
                        cJSON_Delete(body2);
                        if (json2) {
                            char *resp2 = tg_api_call("sendMessage", json2);
                            free(json2);
                            free(resp2);
                        }
                    } else {
                        cJSON_Delete(root);
                        free(resp);
                    }
                } else {
                    free(resp);
                }
            }
        }

        offset += chunk;
    }

    return ESP_OK;
}

esp_err_t telegram_set_token(const char *token)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_TG, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_TG_TOKEN, token));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_bot_token, token, sizeof(s_bot_token) - 1);
    ESP_LOGI(TAG, "Telegram bot token saved");
    return ESP_OK;
}
