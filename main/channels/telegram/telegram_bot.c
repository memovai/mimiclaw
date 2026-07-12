#include "telegram_bot.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "telegram";

static char s_bot_token[128] = MIMI_SECRET_TG_TOKEN;
static char s_groq_key[128] = MIMI_SECRET_GROQ_KEY;
static int64_t s_update_offset = 0;
static int64_t s_last_saved_offset = -1;
static int64_t s_last_offset_save_us = 0;

#define TG_OFFSET_NVS_KEY            "update_offset"
#define TG_DEDUP_CACHE_SIZE          64
#define TG_OFFSET_SAVE_INTERVAL_US   (5LL * 1000 * 1000)
#define TG_OFFSET_SAVE_STEP          10
#define TG_MAX_CONSECUTIVE_FAILURES  5

static uint64_t s_seen_msg_keys[TG_DEDUP_CACHE_SIZE] = {0};
static size_t s_seen_msg_idx = 0;

/* HTTP response accumulator */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    if (!s) {
        return h;
    }
    while (*s) {
        h ^= (unsigned char)(*s++);
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t make_msg_key(const char *chat_id, int msg_id)
{
    uint64_t h = fnv1a64(chat_id);
    return (h << 16) ^ (uint64_t)(msg_id & 0xFFFF) ^ ((uint64_t)msg_id << 32);
}

static bool seen_msg_contains(uint64_t key)
{
    for (size_t i = 0; i < TG_DEDUP_CACHE_SIZE; i++) {
        if (s_seen_msg_keys[i] == key) {
            return true;
        }
    }
    return false;
}

static void seen_msg_insert(uint64_t key)
{
    s_seen_msg_keys[s_seen_msg_idx] = key;
    s_seen_msg_idx = (s_seen_msg_idx + 1) % TG_DEDUP_CACHE_SIZE;
}

static void save_update_offset_if_needed(bool force)
{
    if (s_update_offset <= 0) {
        return;
    }

    int64_t now = esp_timer_get_time();
    bool should_save = force;
    if (!should_save && s_last_saved_offset >= 0) {
        if ((s_update_offset - s_last_saved_offset) >= TG_OFFSET_SAVE_STEP) {
            should_save = true;
        } else if ((now - s_last_offset_save_us) >= TG_OFFSET_SAVE_INTERVAL_US) {
            should_save = true;
        }
    } else if (!should_save) {
        should_save = true;
    }

    if (!should_save) {
        return;
    }

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_TG, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }

    if (nvs_set_i64(nvs, TG_OFFSET_NVS_KEY, s_update_offset) == ESP_OK) {
        if (nvs_commit(nvs) == ESP_OK) {
            s_last_saved_offset = s_update_offset;
            s_last_offset_save_us = now;
        }
    }
    nvs_close(nvs);
}

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

static uint8_t *https_get_bytes_direct(const char *url, size_t *out_len)
{
    *out_len = 0;
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
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buf);
        return NULL;
    }
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || resp.len == 0) {
        ESP_LOGE(TAG, "HTTP download failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return NULL;
    }
    *out_len = resp.len;
    return (uint8_t *)resp.buf;
}

static uint8_t *tg_download_file(const char *file_path, size_t *out_len)
{
    char url[384];
    snprintf(url, sizeof(url), "https://api.telegram.org/file/bot%s/%s", s_bot_token, file_path);

    if (!http_proxy_is_enabled()) {
        return https_get_bytes_direct(url, out_len);
    }

    proxy_conn_t *conn = proxy_conn_open("api.telegram.org", 443, 60000);
    if (!conn) return NULL;

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "GET /file/bot%s/%s HTTP/1.1\r\n"
        "Host: api.telegram.org\r\n"
        "Connection: close\r\n\r\n",
        s_bot_token, file_path);

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return NULL;
    }

    size_t cap = 4096, len = 0;
    char *buf = calloc(1, cap);
    if (!buf) {
        proxy_conn_close(conn);
        return NULL;
    }
    while (1) {
        if (len + 2048 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) break;
            buf = tmp;
        }
        int n = proxy_conn_read(conn, buf + len, cap - len - 1, 60000);
        if (n <= 0) break;
        len += n;
        if (len > MIMI_TG_VOICE_MAX_BYTES + 2048) break;
    }
    proxy_conn_close(conn);

    char *body = strstr(buf, "\r\n\r\n");
    if (!body) {
        free(buf);
        return NULL;
    }
    body += 4;
    size_t body_len = len - (size_t)(body - buf);
    memmove(buf, body, body_len);
    *out_len = body_len;
    return (uint8_t *)buf;
}

static char *tg_get_file_path(const char *file_id)
{
    char method[256];
    snprintf(method, sizeof(method), "getFile?file_id=%s", file_id);
    char *resp = tg_api_call(method, NULL);
    if (!resp) return NULL;

    char *path = NULL;
    cJSON *root = cJSON_Parse(resp);
    if (root) {
        cJSON *result = cJSON_GetObjectItem(root, "result");
        cJSON *file_path = result ? cJSON_GetObjectItem(result, "file_path") : NULL;
        if (cJSON_IsString(file_path)) {
            path = strdup(file_path->valuestring);
        }
        cJSON_Delete(root);
    }
    free(resp);
    return path;
}

static char *groq_api_call(const uint8_t *audio, size_t audio_len)
{
    const char *boundary = "----mimiclaw-groq";
    const char *head_fmt =
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "whisper-large-v3\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"voice.ogg\"\r\n"
        "Content-Type: audio/ogg\r\n\r\n";
    const char *tail_fmt = "\r\n--%s--\r\n";

    int head_len = snprintf(NULL, 0, head_fmt, boundary, boundary);
    int tail_len = snprintf(NULL, 0, tail_fmt, boundary);
    size_t body_len = (size_t)head_len + audio_len + (size_t)tail_len;
    char *body = malloc(body_len + 1);
    if (!body) return NULL;

    snprintf(body, (size_t)head_len + 1, head_fmt, boundary, boundary);
    memcpy(body + head_len, audio, audio_len);
    snprintf(body + head_len + audio_len, (size_t)tail_len + 1, tail_fmt, boundary);

    char ctype[96];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", boundary);

    if (http_proxy_is_enabled()) {
        proxy_conn_t *conn = proxy_conn_open("api.groq.com", 443, 60000);
        if (!conn) {
            free(body);
            return NULL;
        }

        char header[512];
        int hlen = snprintf(header, sizeof(header),
            "POST /openai/v1/audio/transcriptions HTTP/1.1\r\n"
            "Host: api.groq.com\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            s_groq_key, ctype, (int)body_len);

        bool ok = proxy_conn_write(conn, header, hlen) >= 0 &&
                  proxy_conn_write(conn, body, (int)body_len) >= 0;
        free(body);
        if (!ok) {
            proxy_conn_close(conn);
            return NULL;
        }

        size_t cap = 4096, len = 0;
        char *buf = calloc(1, cap);
        if (!buf) {
            proxy_conn_close(conn);
            return NULL;
        }
        while (1) {
            if (len + 1024 >= cap) {
                cap *= 2;
                char *tmp = realloc(buf, cap);
                if (!tmp) break;
                buf = tmp;
            }
            int n = proxy_conn_read(conn, buf + len, cap - len - 1, 60000);
            if (n <= 0) break;
            len += n;
        }
        proxy_conn_close(conn);
        buf[len] = '\0';
        char *json = strstr(buf, "\r\n\r\n");
        char *result = json ? strdup(json + 4) : NULL;
        free(buf);
        return result;
    }

    http_resp_t resp = {
        .buf = calloc(1, 4096),
        .len = 0,
        .cap = 4096,
    };
    if (!resp.buf) {
        free(body);
        return NULL;
    }

    esp_http_client_config_t config = {
        .url = "https://api.groq.com/openai/v1/audio/transcriptions",
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(body);
        free(resp.buf);
        return NULL;
    }
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", s_groq_key);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Content-Type", ctype);
    esp_http_client_set_post_field(client, body, body_len);
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Groq request failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return NULL;
    }
    return resp.buf;
}

static char *transcribe_voice_file_id(const char *file_id)
{
    if (s_groq_key[0] == '\0') {
        ESP_LOGW(TAG, "Drop voice: no Groq key. Use CLI: set_groq_key <KEY>");
        return NULL;
    }

    char *file_path = tg_get_file_path(file_id);
    if (!file_path) {
        ESP_LOGW(TAG, "Telegram getFile failed for voice");
        return NULL;
    }

    size_t audio_len = 0;
    uint8_t *audio = tg_download_file(file_path, &audio_len);
    free(file_path);
    if (!audio) {
        ESP_LOGW(TAG, "Telegram voice download failed");
        return NULL;
    }
    if (audio_len > MIMI_TG_VOICE_MAX_BYTES) {
        ESP_LOGW(TAG, "Drop voice: %d bytes exceeds limit", (int)audio_len);
        free(audio);
        return NULL;
    }

    ESP_LOGI(TAG, "Transcribing Telegram voice (%d bytes)", (int)audio_len);
    char *resp = groq_api_call(audio, audio_len);
    free(audio);
    if (!resp) return NULL;

    char *text = NULL;
    cJSON *root = cJSON_Parse(resp);
    if (root) {
        cJSON *text_item = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text_item)) {
            text = strdup(text_item->valuestring);
        } else {
            cJSON *error = cJSON_GetObjectItem(root, "error");
            char *err_json = error ? cJSON_PrintUnformatted(error) : NULL;
            ESP_LOGW(TAG, "Groq transcription failed: %.160s", err_json ? err_json : resp);
            free(err_json);
        }
        cJSON_Delete(root);
    }
    free(resp);
    return text;
}

static bool tg_response_is_ok(const char *resp, const char **out_desc)
{
    if (out_desc) {
        *out_desc = NULL;
    }
    if (!resp) {
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    if (root) {
        cJSON *ok_field = cJSON_GetObjectItem(root, "ok");
        bool ok = cJSON_IsTrue(ok_field);
        if (!ok && out_desc) {
            cJSON *desc = cJSON_GetObjectItem(root, "description");
            if (desc && cJSON_IsString(desc)) {
                *out_desc = desc->valuestring;
            }
        }
        cJSON_Delete(root);
        return ok;
    }

    /* Proxy or gateway can occasionally return non-standard payload framing. */
    if (strstr(resp, "\"ok\":true") != NULL) {
        return true;
    }

    return false;
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
        /* Track offset and skip stale/duplicate updates */
        cJSON *update_id = cJSON_GetObjectItem(update, "update_id");
        int64_t uid = -1;
        if (cJSON_IsNumber(update_id)) {
            uid = (int64_t)update_id->valuedouble;
        }
        if (uid >= 0) {
            if (uid < s_update_offset) {
                continue;
            }
            s_update_offset = uid + 1;
            save_update_offset_if_needed(false);
        }

        /* Extract message */
        cJSON *message = cJSON_GetObjectItem(update, "message");
        if (!message) continue;

        cJSON *chat = cJSON_GetObjectItem(message, "chat");
        if (!chat) continue;

        cJSON *chat_id = cJSON_GetObjectItem(chat, "id");
        if (!chat_id) continue;

        int msg_id_val = -1;
        cJSON *message_id = cJSON_GetObjectItem(message, "message_id");
        if (cJSON_IsNumber(message_id)) {
            msg_id_val = (int)message_id->valuedouble;
        }

        char chat_id_str[32];
        if (cJSON_IsString(chat_id) && chat_id->valuestring) {
            strncpy(chat_id_str, chat_id->valuestring, sizeof(chat_id_str) - 1);
            chat_id_str[sizeof(chat_id_str) - 1] = '\0';
        } else if (cJSON_IsNumber(chat_id)) {
            snprintf(chat_id_str, sizeof(chat_id_str), "%.0f", chat_id->valuedouble);
        } else {
            continue;
        }

        if (msg_id_val >= 0) {
            uint64_t msg_key = make_msg_key(chat_id_str, msg_id_val);
            if (seen_msg_contains(msg_key)) {
                ESP_LOGW(TAG, "Drop duplicate message update_id=%" PRId64 " chat=%s message_id=%d",
                         uid, chat_id_str, msg_id_val);
                continue;
            }
            seen_msg_insert(msg_key);
        }

        char *owned_content = NULL;
        const char *content = NULL;
        cJSON *text = cJSON_GetObjectItem(message, "text");
        if (cJSON_IsString(text)) {
            content = text->valuestring;
        } else {
            cJSON *voice = cJSON_GetObjectItem(message, "voice");
            cJSON *file_id = voice ? cJSON_GetObjectItem(voice, "file_id") : NULL;
            if (cJSON_IsString(file_id)) {
                cJSON *file_size = cJSON_GetObjectItem(voice, "file_size");
                if (cJSON_IsNumber(file_size) && file_size->valuedouble > MIMI_TG_VOICE_MAX_BYTES) {
                    ESP_LOGW(TAG, "Drop voice: %.0f bytes exceeds limit", file_size->valuedouble);
                    telegram_send_message(chat_id_str, "语音太长了，先发短一点的试试。");
                    continue;
                }
                /* ponytail: one in-memory OGG upload; stream chunks if long voice notes matter. */
                owned_content = transcribe_voice_file_id(file_id->valuestring);
                content = owned_content;
                if (!content) {
                    telegram_send_message(chat_id_str, "语音转写失败了，稍后再试一次。");
                }
            }
        }
        if (!content || !content[0]) {
            free(owned_content);
            continue;
        }

        ESP_LOGI(TAG, "Message update_id=%" PRId64 " message_id=%d from chat %s: %.40s...",
                 uid, msg_id_val, chat_id_str, content);

        /* Push to inbound bus */
        mimi_msg_t msg = {0};
        strncpy(msg.channel, MIMI_CHAN_TELEGRAM, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, chat_id_str, sizeof(msg.chat_id) - 1);
        msg.content = owned_content ? owned_content : strdup(content);
        if (msg.content) {
            if (message_bus_push_inbound(&msg) != ESP_OK) {
                ESP_LOGW(TAG, "Inbound queue full, drop telegram message");
                free(msg.content);
            }
            owned_content = NULL;
        }
        free(owned_content);
    }

    cJSON_Delete(root);
}

static void telegram_poll_task(void *arg)
{
    ESP_LOGI(TAG, "Telegram polling task started");
    int consecutive_failures = 0;

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
            consecutive_failures = 0;
            process_updates(resp);
            free(resp);
        } else {
            consecutive_failures++;
            ESP_LOGW(TAG, "Telegram poll failed (%d/%d)",
                     consecutive_failures, TG_MAX_CONSECUTIVE_FAILURES);
            if (consecutive_failures >= TG_MAX_CONSECUTIVE_FAILURES) {
                ESP_LOGE(TAG, "Telegram polling unhealthy, restarting device");
                esp_restart();
            }
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
        len = sizeof(tmp);
        memset(tmp, 0, sizeof(tmp));
        if (nvs_get_str(nvs, MIMI_NVS_KEY_GROQ_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_groq_key, tmp, sizeof(s_groq_key) - 1);
        }

        int64_t offset = 0;
        if (nvs_get_i64(nvs, TG_OFFSET_NVS_KEY, &offset) == ESP_OK && offset > 0) {
            s_update_offset = offset;
            s_last_saved_offset = offset;
            ESP_LOGI(TAG, "Loaded Telegram update offset: %" PRId64, s_update_offset);
        }
        nvs_close(nvs);
    }

    /* s_bot_token is already initialized from MIMI_SECRET_TG_TOKEN as fallback */

    if (s_bot_token[0]) {
        ESP_LOGI(TAG, "Telegram bot token loaded (len=%d)", (int)strlen(s_bot_token));
    } else {
        ESP_LOGW(TAG, "No Telegram bot token. Use CLI: set_tg_token <TOKEN>");
    }
    if (s_groq_key[0]) {
        ESP_LOGI(TAG, "Groq key loaded for Telegram voice");
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
    int all_ok = 1;

    while (offset < text_len) {
        size_t chunk = text_len - offset;
        if (chunk > MIMI_TG_MAX_MSG_LEN) {
            chunk = MIMI_TG_MAX_MSG_LEN;
        }

        /* Build JSON body */
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "chat_id", chat_id);

        /* Create null-terminated chunk */
        char *segment = malloc(chunk + 1);
        if (!segment) {
            cJSON_Delete(body);
            return ESP_ERR_NO_MEM;
        }
        memcpy(segment, text + offset, chunk);
        segment[chunk] = '\0';

        cJSON_AddStringToObject(body, "text", segment);
        cJSON_AddStringToObject(body, "parse_mode", "Markdown");

        char *json_str = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        free(segment);

        if (!json_str) {
            all_ok = 0;
            offset += chunk;
            continue;
        }

        ESP_LOGI(TAG, "Sending telegram chunk to %s (%d bytes)", chat_id, (int)chunk);
        char *resp = tg_api_call("sendMessage", json_str);
        free(json_str);

        int sent_ok = 0;
        bool markdown_failed = false;
        if (resp) {
            const char *desc = NULL;
            sent_ok = tg_response_is_ok(resp, &desc);
            if (!sent_ok) {
                markdown_failed = true;
                ESP_LOGI(TAG, "Markdown rejected by Telegram for %s: %s",
                         chat_id, desc ? desc : "unknown");
            }
        }

        if (!sent_ok) {
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
                if (resp2) {
                    const char *desc2 = NULL;
                    sent_ok = tg_response_is_ok(resp2, &desc2);
                    if (!sent_ok) {
                        ESP_LOGE(TAG, "Plain send failed: %s", desc2 ? desc2 : "unknown");
                        ESP_LOGE(TAG, "Telegram raw response: %.300s", resp2);
                    }
                    free(resp2);
                } else {
                    ESP_LOGE(TAG, "Plain send failed: no HTTP response");
                }
            } else {
                ESP_LOGE(TAG, "Plain send failed: no JSON body");
            }
        }

        if (!sent_ok) {
            all_ok = 0;
        } else {
            if (markdown_failed) {
                ESP_LOGI(TAG, "Plain-text fallback succeeded for %s", chat_id);
            }
            ESP_LOGI(TAG, "Telegram send success to %s (%d bytes)", chat_id, (int)chunk);
        }

        free(resp);
        offset += chunk;
    }

    return all_ok ? ESP_OK : ESP_FAIL;
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

esp_err_t telegram_set_groq_key(const char *key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_TG, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_GROQ_KEY, key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_groq_key, key, sizeof(s_groq_key) - 1);
    ESP_LOGI(TAG, "Groq key saved");
    return ESP_OK;
}
