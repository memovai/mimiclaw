#include "imessage_bot.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"
#include "mbedtls/base64.h"

static const char *TAG = "imessage";

/* Credentials and proxy URL */
static char s_server_url[256] = MIMI_SECRET_IMSG_SERVER_URL;  /* upstream server, for token only */
static char s_api_key[128]    = MIMI_SECRET_IMSG_API_KEY;
static char s_proxy_url[256]  = MIMI_SECRET_IMSG_PROXY_URL;   /* REST proxy base URL */
static char s_proxy_host[128] = "";                            /* derived from s_proxy_url */
static int  s_proxy_port     = 443;                            /* derived from s_proxy_url */
static char s_proxy_base_path[128] = "/";                      /* derived from s_proxy_url */
static char s_auth_token[512] = "";  /* Base64 of "server_url|api_key" */

/* Lock protecting credential strings and poller state against concurrent
 * access from the poll task, send path, and credential-update path. */
static SemaphoreHandle_t s_cred_lock = NULL;

/* Polling state: track the newest message date we've seen */
static double s_last_seen_date = 0;

/* Re-arm flag: when credentials change at runtime the poller must re-run
 * the first-poll baseline logic.  Moved out of task-local scope so that
 * imessage_set_credentials() can set it. */
static volatile bool s_first_poll = true;

/* Dedup ring buffer (same pattern as Telegram) */
#define IMSG_DEDUP_CACHE_SIZE  64
static uint64_t s_seen_msg_keys[IMSG_DEDUP_CACHE_SIZE] = {0};
static size_t s_seen_msg_idx = 0;

/* Known-contacts set: tracks chat_ids from which we have received at least
 * one inbound message.  Apple silently drops outbound iMessages to addresses
 * that have never contacted us first (spam filtering), so we gate sends on
 * this set.  Uses a flat hash table (open addressing, linear probe). */
#define IMSG_KNOWN_CONTACTS_SIZE  128
// 0 is empty
static uint64_t s_known_contacts[IMSG_KNOWN_CONTACTS_SIZE] = {0};
static size_t   s_known_contacts_count = 0;

/* HTTP response accumulator */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

/* ── Helpers ──────────────────────────────────────────────────── */

/*
 * Parse s_proxy_url ("https://host:port/base/path") into:
 *   s_proxy_host      – hostname only (e.g. "proxy.example.com")
 *   s_proxy_port      – port number (defaults to 443 for https, 80 for http)
 *   s_proxy_base_path – URL path with trailing slash (e.g. "/api/v1/")
 */
static void derive_proxy_host(void)
{
    s_proxy_host[0] = '\0';
    s_proxy_port = 443;
    s_proxy_base_path[0] = '/';
    s_proxy_base_path[1] = '\0';

    const char *p = s_proxy_url;

    /* Detect scheme and set default port accordingly */
    const char *scheme_end = strstr(p, "://");
    if (scheme_end) {
        if (strncmp(p, "http://", 7) == 0) {
            s_proxy_port = 80;
        }
        p = scheme_end + 3;
    }

    /* Extract hostname (up to ':', '/', or end) */
    size_t i = 0;
    while (*p && *p != '/' && *p != ':' && i < sizeof(s_proxy_host) - 1) {
        s_proxy_host[i++] = *p++;
    }
    s_proxy_host[i] = '\0';

    /* Extract port if present */
    if (*p == ':') {
        p++;
        int port = 0;
        while (*p >= '0' && *p <= '9') {
            port = port * 10 + (*p++ - '0');
        }
        if (port > 0 && port <= 65535) {
            s_proxy_port = port;
        }
    }

    /* Extract base path (everything from '/' onward, or "/" if none) */
    if (*p == '/') {
        strncpy(s_proxy_base_path, p, sizeof(s_proxy_base_path) - 1);
        s_proxy_base_path[sizeof(s_proxy_base_path) - 1] = '\0';
        /* Ensure trailing slash */
        size_t path_len = strlen(s_proxy_base_path);
        if (path_len > 0 && s_proxy_base_path[path_len - 1] != '/'
            && path_len < sizeof(s_proxy_base_path) - 1) {
            s_proxy_base_path[path_len] = '/';
            s_proxy_base_path[path_len + 1] = '\0';
        }
    }
}

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    if (!s) return h;
    while (*s) {
        h ^= (unsigned char)(*s++);
        h *= 1099511628211ULL;
    }
    return h;
}

static bool seen_msg_contains(uint64_t key)
{
    for (size_t i = 0; i < IMSG_DEDUP_CACHE_SIZE; i++) {
        if (s_seen_msg_keys[i] == key) return true;
    }
    return false;
}

static void seen_msg_insert(uint64_t key)
{
    s_seen_msg_keys[s_seen_msg_idx] = key;
    s_seen_msg_idx = (s_seen_msg_idx + 1) % IMSG_DEDUP_CACHE_SIZE;
}

/* Known-contacts helpers (open-addressing hash set).
 * Slot 0 is reserved as "empty", so we map any zero hash to 1. */

static void known_contacts_save(void);  /* forward decl – defined after clear() */

static uint64_t contact_hash(const char *chat_id)
{
    uint64_t h = fnv1a64(chat_id);
    return h ? h : 1;  /* should never be 0... empty slot */
}

static bool known_contact_contains(const char *chat_id)
{
    uint64_t h = contact_hash(chat_id);
    for (size_t i = 0; i < IMSG_KNOWN_CONTACTS_SIZE; i++) {
        size_t idx = (h + i) % IMSG_KNOWN_CONTACTS_SIZE;
        if (s_known_contacts[idx] == h) return true;
        if (s_known_contacts[idx] == 0) return false;  /* empty slot → miss */
    }
    return false;
}

static void known_contact_insert(const char *chat_id)
{
    uint64_t h = contact_hash(chat_id);
    for (size_t i = 0; i < IMSG_KNOWN_CONTACTS_SIZE; i++) {
        size_t idx = (h + i) % IMSG_KNOWN_CONTACTS_SIZE;
        if (s_known_contacts[idx] == h) return;  /* already present */
        if (s_known_contacts[idx] == 0) {
            if (s_known_contacts_count >= IMSG_KNOWN_CONTACTS_SIZE * 3 / 4) {
                ESP_LOGW(TAG, "Known-contacts set is full, cannot track new contact");
                return;
            }
            s_known_contacts[idx] = h;
            s_known_contacts_count++;
            known_contacts_save();
            return;
        }
    }
}

static void known_contacts_clear(void)
{
    memset(s_known_contacts, 0, sizeof(s_known_contacts));
    s_known_contacts_count = 0;

    /* Erase the persisted blob so stale contacts from a previous account
     * don't reappear after reboot. */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_IMSG, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, MIMI_NVS_KEY_IMSG_CONTACTS);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

/* ── NVS persistence for known-contacts set ──────────────────── */

/* Save the hash table + count as a single NVS blob.
 * Layout: [uint64_t × IMSG_KNOWN_CONTACTS_SIZE] [size_t count]
 * Total: 128 × 8 + sizeof(size_t) = 1028 bytes on ESP32 (size_t == 4). */
static void known_contacts_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_IMSG, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "known_contacts_save: cannot open NVS");
        return;
    }
    /* Pack table + count into one contiguous buffer */
    uint8_t blob[sizeof(s_known_contacts) + sizeof(s_known_contacts_count)];
    memcpy(blob, s_known_contacts, sizeof(s_known_contacts));
    memcpy(blob + sizeof(s_known_contacts), &s_known_contacts_count,
           sizeof(s_known_contacts_count));

    esp_err_t err = nvs_set_blob(nvs, MIMI_NVS_KEY_IMSG_CONTACTS, blob, sizeof(blob));
    if (err == ESP_OK) {
        nvs_commit(nvs);
    } else {
        ESP_LOGW(TAG, "known_contacts_save: nvs_set_blob failed: %s", esp_err_to_name(err));
    }
    nvs_close(nvs);
}

/* Load the known-contacts hash table from NVS.  Called once at init. */
static void known_contacts_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_IMSG, NVS_READONLY, &nvs) != ESP_OK) {
        return;  /* no NVS data yet – table stays zeroed */
    }

    size_t expected = sizeof(s_known_contacts) + sizeof(s_known_contacts_count);
    size_t len = 0;
    esp_err_t err = nvs_get_blob(nvs, MIMI_NVS_KEY_IMSG_CONTACTS, NULL, &len);
    if (err != ESP_OK || len != expected) {
        nvs_close(nvs);
        return;  /* key missing or size mismatch – start fresh */
    }

    uint8_t blob[expected];
    err = nvs_get_blob(nvs, MIMI_NVS_KEY_IMSG_CONTACTS, blob, &len);
    nvs_close(nvs);
    if (err != ESP_OK) return;

    memcpy(s_known_contacts, blob, sizeof(s_known_contacts));
    memcpy(&s_known_contacts_count, blob + sizeof(s_known_contacts),
           sizeof(s_known_contacts_count));

    ESP_LOGI(TAG, "Loaded %d known contacts from NVS", (int)s_known_contacts_count);
}

/*
 * Build the Bearer token: base64("server_url|api_key")
 */
static void build_auth_token(void)
{
    if (s_server_url[0] == '\0' || s_api_key[0] == '\0') {
        s_auth_token[0] = '\0';
        return;
    }

    /* Build "server_url|api_key" */
    char raw[384];
    int raw_len = snprintf(raw, sizeof(raw), "%s|%s", s_server_url, s_api_key);
    if (raw_len <= 0 || raw_len >= (int)sizeof(raw)) {
        ESP_LOGE(TAG, "Credentials too long for auth token");
        s_auth_token[0] = '\0';
        return;
    }

    size_t olen = 0;
    int ret = mbedtls_base64_encode(
        (unsigned char *)s_auth_token, sizeof(s_auth_token) - 1,
        &olen, (const unsigned char *)raw, (size_t)raw_len);

    if (ret != 0) {
        ESP_LOGE(TAG, "Base64 encode failed: %d", ret);
        s_auth_token[0] = '\0';
        return;
    }
    s_auth_token[olen] = '\0';
    ESP_LOGI(TAG, "Auth token built (len=%d)", (int)olen);
}

/* ── HTTP helpers ─────────────────────────────────────────────── */

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

/*
 * Make an HTTP request to the iMessage proxy.
 * method      - HTTP method path (e.g. "messages?limit=20")
 * http_method - HTTP_METHOD_GET or HTTP_METHOD_POST
 * post_data   - JSON body for POST, or NULL for GET
 * Returns heap-allocated response body, or NULL on error. Caller must free().
 */
static char *imsg_api_call(const char *method, esp_http_client_method_t http_method,
                           const char *post_data)
{
    /* Snapshot credentials under lock so we don't race set_credentials(). */
    char local_proxy_url[256];
    char local_auth_token[512];

    xSemaphoreTake(s_cred_lock, portMAX_DELAY);
    if (s_auth_token[0] == '\0') {
        xSemaphoreGive(s_cred_lock);
        ESP_LOGW(TAG, "No auth token, skipping API call");
        return NULL;
    }
    memcpy(local_proxy_url, s_proxy_url, sizeof(local_proxy_url));
    memcpy(local_auth_token, s_auth_token, sizeof(local_auth_token));
    xSemaphoreGive(s_cred_lock);

    /* All REST calls go to the centrally-hosted proxy, NOT the upstream
     * iMessage Kit server. The upstream URL is only encoded in the Bearer token. */
    char url[512];
    snprintf(url, sizeof(url), "%s/%s", local_proxy_url, method);

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
        .timeout_ms = MIMI_IMSG_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for %s", method);
        free(resp.buf);
        return NULL;
    }

    esp_http_client_set_method(client, http_method);

    /* Set authorization header (use snapshotted token) */
    char auth_header[560];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", local_auth_token);
    esp_http_client_set_header(client, "Authorization", auth_header);

    if (post_data) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
    }

    ESP_LOGD(TAG, "HTTP %s %s (timeout=%dms)",
             (http_method == HTTP_METHOD_POST) ? "POST" : "GET",
             method, MIMI_IMSG_HTTP_TIMEOUT_MS);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGD(TAG, "HTTP response: status=%d err=%s len=%d",
             status, esp_err_to_name(err), (int)resp.len);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request to %s failed: %s", method, esp_err_to_name(err));
        free(resp.buf);
        return NULL;
    }

    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "HTTP %d from %s: %.200s", status, method, resp.buf);
    }

    return resp.buf;
}

/* ── Proxy path: manual HTTP over CONNECT tunnel ────────────── */

static char *imsg_api_call_via_proxy(const char *method, esp_http_client_method_t http_method,
                                     const char *post_data)
{
    /* Snapshot credentials under lock so we don't race set_credentials(). */
    char local_proxy_host[128];
    char local_auth_token[512];
    char local_base_path[128];
    int  local_port;

    xSemaphoreTake(s_cred_lock, portMAX_DELAY);
    memcpy(local_proxy_host, s_proxy_host, sizeof(local_proxy_host));
    memcpy(local_auth_token, s_auth_token, sizeof(local_auth_token));
    memcpy(local_base_path, s_proxy_base_path, sizeof(local_base_path));
    local_port = s_proxy_port;
    xSemaphoreGive(s_cred_lock);

    /* All REST calls go to the centrally-hosted proxy */
    const char *host = local_proxy_host;

    proxy_conn_t *conn = proxy_conn_open(host, local_port, MIMI_IMSG_HTTP_TIMEOUT_MS);
    if (!conn) return NULL;

    /* Build auth header (use snapshotted token) */
    char auth_header[560];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", local_auth_token);

    /* Build the full request path: base_path + method (base_path already has
     * a trailing slash, e.g. "/api/v1/" + "messages?limit=20"). */
    char req_path[384];
    snprintf(req_path, sizeof(req_path), "%s%s", local_base_path, method);

    /* Build HTTP request */
    char header[1024];
    int header_len;
    const char *method_str = (http_method == HTTP_METHOD_POST) ? "POST" : "GET";

    if (post_data) {
        header_len = snprintf(header, sizeof(header),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Authorization: %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            method_str, req_path, host, auth_header, (int)strlen(post_data));
    } else {
        header_len = snprintf(header, sizeof(header),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Authorization: %s\r\n"
            "Connection: close\r\n\r\n",
            method_str, req_path, host, auth_header);
    }

    if (proxy_conn_write(conn, header, header_len) < 0) {
        proxy_conn_close(conn);
        return NULL;
    }
    if (post_data && proxy_conn_write(conn, post_data, strlen(post_data)) < 0) {
        proxy_conn_close(conn);
        return NULL;
    }

    /* Read response */
    size_t cap = 4096, len = 0;
    char *buf = calloc(1, cap);
    if (!buf) { proxy_conn_close(conn); return NULL; }

    while (1) {
        if (len + 1024 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) break;
            buf = tmp;
        }
        int n = proxy_conn_read(conn, buf + len, cap - len - 1, MIMI_IMSG_HTTP_TIMEOUT_MS);
        if (n <= 0) break;
        len += n;
    }
    buf[len] = '\0';
    proxy_conn_close(conn);

    /* Skip HTTP headers */
    char *body = strstr(buf, "\r\n\r\n");
    if (!body) { free(buf); return NULL; }
    body += 4;

    char *result = strdup(body);
    free(buf);
    return result;
}

/*
 * Unified API call: uses proxy tunnel if configured, otherwise direct.
 */
static char *imsg_call(const char *method, esp_http_client_method_t http_method,
                       const char *post_data)
{
    if (http_proxy_is_enabled()) {
        return imsg_api_call_via_proxy(method, http_method, post_data);
    }
    return imsg_api_call(method, http_method, post_data);
}

/* ── Message processing ───────────────────────────────────────── */

/*
 * Determine the chat_id for the message bus from a proxy message object.
 *
 * Proxy message format:
 *   {"id":"...","text":"...","from":"user@icloud.com","chat":"group:abc123","sentAt":timestamp}
 *
 * - For 1:1 chats: "from" is the sender address, "chat" is null or the address
 * - For group chats: "chat" starts with "group:"
 * - We reply to "from" for 1:1, or to "chat" for groups
 */
static bool extract_chat_id(cJSON *msg, char *out, size_t out_size)
{
    /* Check "chat" field first -- for group chats it will be "group:xxx" */
    cJSON *chat = cJSON_GetObjectItem(msg, "chat");
    if (cJSON_IsString(chat) && chat->valuestring && chat->valuestring[0]) {
        strncpy(out, chat->valuestring, out_size - 1);
        out[out_size - 1] = '\0';
        return true;
    }

    /* For 1:1 chats, use "from" (sender address) */
    cJSON *from = cJSON_GetObjectItem(msg, "from");
    if (cJSON_IsString(from) && from->valuestring && from->valuestring[0]) {
        strncpy(out, from->valuestring, out_size - 1);
        out[out_size - 1] = '\0';
        return true;
    }

    return false;
}

/*
 * Process the response from GET /messages and push new messages to inbound bus.
 *
 * Proxy message format:
 *   {"ok":true,"data":[
 *     {"id":"UUID","text":"Hello","from":"user@icloud.com","chat":null,"sentAt":1772701661223},
 *     ...
 *   ]}
 *
 * Fields:
 *   id      - message UUID (string)
 *   text    - message text (string, may be null for attachments)
 *   from    - sender address (string, null/absent if sent by us)
 *   chat    - chat identifier (string or null; "group:xxx" for groups)
 *   sentAt  - timestamp in milliseconds (number)
 */
static void process_messages(const char *json_str)
{
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse messages JSON");
        return;
    }

    /* Envelope: {"ok": true, "data": [...]} */
    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    if (!cJSON_IsTrue(ok)) {
        cJSON *error = cJSON_GetObjectItem(root, "error");
        if (cJSON_IsObject(error)) {
            cJSON *emsg = cJSON_GetObjectItem(error, "message");
            ESP_LOGE(TAG, "API error: %s",
                     cJSON_IsString(emsg) ? emsg->valuestring : "unknown");
        }
        cJSON_Delete(root);
        return;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsArray(data)) {
        cJSON_Delete(root);
        return;
    }

    /* Messages are returned newest-first; iterate in reverse so we process oldest first */
    int count = cJSON_GetArraySize(data);

    for (int i = count - 1; i >= 0; i--) {
        cJSON *msg = cJSON_GetArrayItem(data, i);
        if (!msg) continue;

        /* Get message ID for dedup — needed before any skip checks */
        cJSON *id = cJSON_GetObjectItem(msg, "id");
        if (!cJSON_IsString(id) || !id->valuestring) continue;

        uint64_t msg_key = fnv1a64(id->valuestring);
        if (seen_msg_contains(msg_key)) continue;

        /* Get sentAt timestamp */
        cJSON *sent_at = cJSON_GetObjectItem(msg, "sentAt");
        double msg_date = 0;
        if (cJSON_IsNumber(sent_at)) {
            msg_date = sent_at->valuedouble;
        }

        /* Skip messages older than our high-water mark */
        if (msg_date > 0 && msg_date <= s_last_seen_date) continue;

        /* Advance dedup ring and high-water mark BEFORE the content/sender
         * checks so that self-sent, attachment-only, or otherwise skipped
         * messages still move the stream cursor forward.  Without this the
         * poller re-reads the same newest page indefinitely when the most
         * recent message is one we skip. */
        seen_msg_insert(msg_key);
        if (msg_date > s_last_seen_date) {
            s_last_seen_date = msg_date;
        }

        /* Skip messages we sent: "from" is "me" for our own messages */
        cJSON *from = cJSON_GetObjectItem(msg, "from");
        if (!cJSON_IsString(from) || !from->valuestring || from->valuestring[0] == '\0') continue;
        if (strcmp(from->valuestring, "me") == 0) continue;

        /* Extract text content */
        cJSON *text = cJSON_GetObjectItem(msg, "text");
        if (!cJSON_IsString(text) || !text->valuestring || text->valuestring[0] == '\0') continue;

        /* Extract chat ID (sender address or group ID) */
        char chat_id[96];
        if (!extract_chat_id(msg, chat_id, sizeof(chat_id))) {
            ESP_LOGW(TAG, "Cannot determine chat_id for message %s", id->valuestring);
            continue;
        }

        ESP_LOGI(TAG, "New iMessage from %s: %.40s...", chat_id, text->valuestring);

        /* Track this chat_id as a known contact so outbound sends are allowed */
        known_contact_insert(chat_id);

        /* Push to inbound bus */
        mimi_msg_t bus_msg = {0};
        strncpy(bus_msg.channel, MIMI_CHAN_IMESSAGE, sizeof(bus_msg.channel) - 1);
        strncpy(bus_msg.chat_id, chat_id, sizeof(bus_msg.chat_id) - 1);
        bus_msg.content = strdup(text->valuestring);
        if (bus_msg.content) {
            if (message_bus_push_inbound(&bus_msg) != ESP_OK) {
                ESP_LOGW(TAG, "Inbound queue full, drop iMessage");
                free(bus_msg.content);
            }
        }
    }

    cJSON_Delete(root);
}

/* ── Polling task ─────────────────────────────────────────────── */

static void imessage_poll_task(void *arg)
{
    ESP_LOGI(TAG, "iMessage polling task started");

    while (1) {
        /* Check credentials under lock */
        xSemaphoreTake(s_cred_lock, portMAX_DELAY);
        bool have_creds = (s_auth_token[0] != '\0');
        bool need_baseline = s_first_poll;
        xSemaphoreGive(s_cred_lock);

        if (!have_creds) {
            ESP_LOGW(TAG, "No iMessage credentials configured, waiting...");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        if (need_baseline) {
            /* On first poll (or after credential change), set the high-water
             * mark to "now" so we don't replay the entire message history. */
            ESP_LOGI(TAG, "First poll: fetching baseline from proxy...");
            char *resp = imsg_call("messages?limit=1", HTTP_METHOD_GET, NULL);
            if (resp) {
                ESP_LOGD(TAG, "First poll response (%d bytes): %.200s", (int)strlen(resp), resp);
                cJSON *root = cJSON_Parse(resp);
                if (root) {
                    cJSON *ok = cJSON_GetObjectItem(root, "ok");
                    cJSON *data = cJSON_GetObjectItem(root, "data");
                    if (cJSON_IsTrue(ok) && cJSON_IsArray(data) && cJSON_GetArraySize(data) > 0) {
                        cJSON *newest = cJSON_GetArrayItem(data, 0);
                        cJSON *sa = cJSON_GetObjectItem(newest, "sentAt");

                        xSemaphoreTake(s_cred_lock, portMAX_DELAY);
                        if (cJSON_IsNumber(sa)) {
                            s_last_seen_date = sa->valuedouble;
                            ESP_LOGI(TAG, "Baseline message date set: %.0f", s_last_seen_date);
                        }
                        /* Also seed the dedup cache with this message */
                        cJSON *mid = cJSON_GetObjectItem(newest, "id");
                        if (cJSON_IsString(mid) && mid->valuestring) {
                            seen_msg_insert(fnv1a64(mid->valuestring));
                        }
                        s_first_poll = false;
                        xSemaphoreGive(s_cred_lock);
                    } else {
                        xSemaphoreTake(s_cred_lock, portMAX_DELAY);
                        s_first_poll = false;
                        xSemaphoreGive(s_cred_lock);
                    }
                    cJSON_Delete(root);
                }
                free(resp);
            } else {
                /* Retry on failure */
                ESP_LOGW(TAG, "First poll failed, retrying...");
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }

        /* Poll for new messages */
        ESP_LOGD(TAG, "Polling for new messages...");
        char *resp = imsg_call("messages?limit=20&sort=desc", HTTP_METHOD_GET, NULL);
        if (resp) {
            ESP_LOGD(TAG, "Poll response (%d bytes)", (int)strlen(resp));
            process_messages(resp);
            free(resp);
        } else {
            ESP_LOGW(TAG, "iMessage poll failed (no HTTP response)");
        }

        vTaskDelay(pdMS_TO_TICKS(MIMI_IMSG_POLL_INTERVAL_MS));
    }
}

/* ── Public API ───────────────────────────────────────────────── */

esp_err_t imessage_bot_init(void)
{
    /* Create the credentials / poller-state mutex */
    if (!s_cred_lock) {
        s_cred_lock = xSemaphoreCreateMutex();
        configASSERT(s_cred_lock);
    }

    /* NVS overrides take highest priority */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_IMSG, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[256] = {0};
        size_t len;

        len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_IMSG_URL, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_server_url, tmp, sizeof(s_server_url) - 1);
        }

        memset(tmp, 0, sizeof(tmp));
        len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_IMSG_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_api_key, tmp, sizeof(s_api_key) - 1);
        }

        memset(tmp, 0, sizeof(tmp));
        len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_IMSG_PROXY_URL, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_proxy_url, tmp, sizeof(s_proxy_url) - 1);
        }

        nvs_close(nvs);
    }

    /* Strip trailing slashes from URLs */
    size_t url_len = strlen(s_server_url);
    if (url_len > 0 && s_server_url[url_len - 1] == '/') {
        s_server_url[url_len - 1] = '\0';
    }
    url_len = strlen(s_proxy_url);
    if (url_len > 0 && s_proxy_url[url_len - 1] == '/') {
        s_proxy_url[url_len - 1] = '\0';
    }

    /* Derive proxy host from URL */
    derive_proxy_host();

    /* Build auth token */
    if (s_server_url[0] && s_api_key[0]) {
        build_auth_token();
        ESP_LOGI(TAG, "iMessage credentials loaded (server=%s proxy=%s)", s_server_url, s_proxy_url);
    } else {
        ESP_LOGW(TAG, "No iMessage credentials. Use CLI: set_imsg_creds <url> <key>");
    }

    /* Restore known-contacts set from NVS so outbound sends work
     * immediately after reboot without waiting for inbound messages. */
    known_contacts_load();

    return ESP_OK;
}

esp_err_t imessage_bot_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        imessage_poll_task, "imsg_poll",
        MIMI_IMSG_POLL_STACK, NULL,
        MIMI_IMSG_POLL_PRIO, NULL, MIMI_IMSG_POLL_CORE);

    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t imessage_send_message(const char *chat_id, const char *text)
{
    xSemaphoreTake(s_cred_lock, portMAX_DELAY);
    bool have_creds = (s_auth_token[0] != '\0');
    xSemaphoreGive(s_cred_lock);

    if (!have_creds) {
        ESP_LOGW(TAG, "Cannot send: no iMessage credentials");
        return ESP_ERR_INVALID_STATE;
    }

    /* Apple silently drops outbound iMessages to addresses that have never
     * contacted us first (spam filtering).  Block the send early and surface
     * a clear error instead of returning a false ESP_OK. */
    if (!known_contact_contains(chat_id)) {
        ESP_LOGW(TAG, "Cannot send to %s: no prior inbound message from this "
                 "address (Apple spam filter would silently drop it)", chat_id);
        return ESP_ERR_NOT_ALLOWED;
    }

    /* Split long messages at MIMI_IMSG_MAX_MSG_LEN boundary */
    size_t text_len = strlen(text);
    size_t offset = 0;
    int all_ok = 1;

    while (offset < text_len) {
        size_t chunk = text_len - offset;
        if (chunk > MIMI_IMSG_MAX_MSG_LEN) {
            chunk = MIMI_IMSG_MAX_MSG_LEN;
        }

        /* Build JSON body */
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "to", chat_id);

        char *segment = malloc(chunk + 1);
        if (!segment) {
            cJSON_Delete(body);
            return ESP_ERR_NO_MEM;
        }
        memcpy(segment, text + offset, chunk);
        segment[chunk] = '\0';

        cJSON_AddStringToObject(body, "text", segment);
        free(segment);

        char *json_str = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);

        if (!json_str) {
            all_ok = 0;
            offset += chunk;
            continue;
        }

        ESP_LOGI(TAG, "Sending iMessage to %s (%d bytes)", chat_id, (int)chunk);
        char *resp = imsg_call("send", HTTP_METHOD_POST, json_str);
        free(json_str);

        int sent_ok = 0;
        if (resp) {
            cJSON *root = cJSON_Parse(resp);
            if (root) {
                cJSON *ok = cJSON_GetObjectItem(root, "ok");
                sent_ok = cJSON_IsTrue(ok);
                if (!sent_ok) {
                    cJSON *error = cJSON_GetObjectItem(root, "error");
                    if (cJSON_IsObject(error)) {
                        cJSON *emsg = cJSON_GetObjectItem(error, "message");
                        ESP_LOGE(TAG, "Send failed: %s",
                                 cJSON_IsString(emsg) ? emsg->valuestring : "unknown");
                    }
                }
                cJSON_Delete(root);
            }
            free(resp);
        }

        if (!sent_ok) {
            ESP_LOGE(TAG, "iMessage send failed for %s", chat_id);
            all_ok = 0;
        } else {
            ESP_LOGI(TAG, "iMessage send success to %s (%d bytes)", chat_id, (int)chunk);
        }

        offset += chunk;
    }

    return all_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t imessage_set_credentials(const char *server_url, const char *api_key,
                                   const char *proxy_url)
{
    /* Persist to NVS first (outside the lock -- NVS has its own locking). */
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_IMSG, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_IMSG_URL, server_url));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_IMSG_API_KEY, api_key));
    if (proxy_url) {
        ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_IMSG_PROXY_URL, proxy_url));
    }
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    /* Swap credentials and reset poller state atomically. */
    xSemaphoreTake(s_cred_lock, portMAX_DELAY);

    strncpy(s_server_url, server_url, sizeof(s_server_url) - 1);
    strncpy(s_api_key, api_key, sizeof(s_api_key) - 1);
    if (proxy_url) {
        strncpy(s_proxy_url, proxy_url, sizeof(s_proxy_url) - 1);
    }

    /* Strip trailing slashes */
    size_t url_len = strlen(s_server_url);
    if (url_len > 0 && s_server_url[url_len - 1] == '/') {
        s_server_url[url_len - 1] = '\0';
    }
    url_len = strlen(s_proxy_url);
    if (url_len > 0 && s_proxy_url[url_len - 1] == '/') {
        s_proxy_url[url_len - 1] = '\0';
    }

    derive_proxy_host();
    build_auth_token();

    /* Reset poller state so the new account starts fresh. */
    s_last_seen_date = 0;
    memset(s_seen_msg_keys, 0, sizeof(s_seen_msg_keys));
    s_seen_msg_idx = 0;
    known_contacts_clear();
    s_first_poll = true;

    xSemaphoreGive(s_cred_lock);

    ESP_LOGI(TAG, "iMessage credentials saved (server=%s proxy=%s)", s_server_url, s_proxy_url);
    return ESP_OK;
}
