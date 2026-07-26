#include "mqtt_bot.h"
#include "mqtt_message.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_crt_bundle.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_wifi.h"

static const char *TAG = "mqtt";

/* ── Configuration state ────────────────────────────────────── */
static char s_broker_uri[256] = {0};
static char s_client_id[64] = {0};
static char s_username[128] = {0};
static char s_password[128] = {0};
static char s_subscribe_topic[128] = {0};

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static bool s_connected = false;
static bool s_enabled = false;
static mqtt_message_assembly_t s_inbound_assembly = {0};

static void dispatch_inbound_message(char *topic, char *data)
{
    /*
     * Process every complete PUBLISH. Repeated topic/payload pairs can be
     * intentional commands, so they are not safe application-level dedup keys.
     */
    ESP_LOGI(TAG, "Received on topic [%s]: %.60s%s",
             topic, data, strlen(data) > 60 ? "..." : "");

    /* Parse JSON payload if present, otherwise treat as plain text. */
    char *content = NULL;
    cJSON *root = cJSON_Parse(data);
    if (root) {
        cJSON *text = cJSON_GetObjectItem(root, "text");
        cJSON *message = cJSON_GetObjectItem(root, "message");
        cJSON *payload_field = cJSON_GetObjectItem(root, "payload");

        const char *extracted = NULL;
        if (text && cJSON_IsString(text)) {
            extracted = text->valuestring;
        } else if (message && cJSON_IsString(message)) {
            extracted = message->valuestring;
        } else if (payload_field && cJSON_IsString(payload_field)) {
            extracted = payload_field->valuestring;
        }

        if (extracted && extracted[0]) {
            content = strdup(extracted);
        }
        cJSON_Delete(root);
    }

    if (!content) {
        content = strdup(data);
    }

    if (content && content[0]) {
        mimi_msg_t msg = {0};
        strncpy(msg.channel, MIMI_CHAN_MQTT, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, topic, sizeof(msg.chat_id) - 1);
        msg.content = content;

        if (message_bus_push_inbound(&msg) != ESP_OK) {
            ESP_LOGW(TAG, "Inbound queue full, dropping MQTT message");
            free(msg.content);
        } else {
            ESP_LOGI(TAG, "Message pushed to inbound bus: %s", topic);
        }
    } else {
        free(content);
    }

    free(topic);
    free(data);
}

/* ── MQTT Event Handler ─────────────────────────────────────── */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)base;
    (void)handler_args;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "MQTT connected to %s", s_broker_uri);

            /* Subscribe to the configured topic pattern */
            if (s_subscribe_topic[0] != '\0') {
                int msg_id = esp_mqtt_client_subscribe(s_mqtt_client, s_subscribe_topic, 1);
                if (msg_id >= 0) {
                    ESP_LOGI(TAG, "Subscribed to topic: %s (msg_id=%d)", s_subscribe_topic, msg_id);
                } else {
                    ESP_LOGW(TAG, "Failed to subscribe to topic: %s", s_subscribe_topic);
                }
            }
            break;

        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            mqtt_message_assembly_reset(&s_inbound_assembly);
            ESP_LOGW(TAG, "MQTT disconnected");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGD(TAG, "MQTT subscribed, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGD(TAG, "MQTT unsubscribed, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGD(TAG, "MQTT published, msg_id=%d", event->msg_id);
            break;

        case MQTT_EVENT_DATA:
            {
                if (event->topic_len < 0 ||
                    event->data_len < 0 ||
                    event->current_data_offset < 0 ||
                    event->total_data_len < 0) {
                    ESP_LOGW(TAG, "Dropping MQTT event with invalid lengths");
                    mqtt_message_assembly_reset(&s_inbound_assembly);
                    break;
                }

                size_t data_len = (size_t)event->data_len;
                size_t total_len = event->total_data_len > 0
                    ? (size_t)event->total_data_len
                    : data_len;
                char *topic = NULL;
                char *data = NULL;

                mqtt_message_assembly_result_t result =
                    mqtt_message_assembly_append(
                        &s_inbound_assembly,
                        event->msg_id,
                        event->topic,
                        (size_t)event->topic_len,
                        event->data,
                        data_len,
                        (size_t)event->current_data_offset,
                        total_len,
                        MIMI_MQTT_MAX_MSG_LEN,
                        &topic,
                        &data);

                if (result == MQTT_MESSAGE_ASSEMBLY_ERROR) {
                    ESP_LOGW(TAG,
                             "Dropping malformed or oversized MQTT message "
                             "(offset=%d, chunk=%d, total=%d)",
                             event->current_data_offset,
                             event->data_len,
                             event->total_data_len);
                    break;
                }

                if (result == MQTT_MESSAGE_ASSEMBLY_COMPLETE) {
                    dispatch_inbound_message(topic, data);
                }
            }
            break;

        case MQTT_EVENT_ERROR:
            mqtt_message_assembly_reset(&s_inbound_assembly);
            ESP_LOGE(TAG, "MQTT error: %d", event->error_handle->error_type);
            break;

        case MQTT_EVENT_BEFORE_CONNECT:
            ESP_LOGI(TAG, "MQTT connecting to %s...", s_broker_uri);
            break;

        default:
            ESP_LOGD(TAG, "MQTT event: %d", event_id);
            break;
    }
}

static void load_default_string(const char *name, const char *value,
                                char *destination, size_t capacity)
{
    destination[0] = '\0';
    if (!value || value[0] == '\0') {
        return;
    }

    if (!mqtt_copy_string(destination, capacity, value)) {
        ESP_LOGE(TAG, "Ignoring oversized build-time %s "
                 "(maximum %u bytes)",
                 name, (unsigned)(capacity - 1));
    }
}

static void load_nvs_string(nvs_handle_t nvs, const char *key,
                            char *destination, size_t capacity)
{
    size_t required = 0;
    esp_err_t err = nvs_get_str(nvs, key, NULL, &required);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to read MQTT setting %s: %s",
                 key, esp_err_to_name(err));
        return;
    }
    if (required == 0 || required > capacity) {
        ESP_LOGW(TAG,
                 "Ignoring oversized MQTT setting %s (%u bytes, maximum %u)",
                 key, (unsigned)required, (unsigned)capacity);
        err = nvs_erase_key(nvs, key);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Unable to clear MQTT setting %s: %s",
                     key, esp_err_to_name(err));
        }
        return;
    }

    char *value = malloc(required);
    if (!value) {
        ESP_LOGW(TAG, "Unable to allocate MQTT setting %s", key);
        return;
    }

    err = nvs_get_str(nvs, key, value, &required);
    if (err == ESP_OK && value[0] != '\0') {
        mqtt_copy_string(destination, capacity, value);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to load MQTT setting %s: %s",
                 key, esp_err_to_name(err));
    }
    free(value);
}

static esp_err_t validate_mqtt_value(const char *name, const char *value,
                                     size_t capacity, bool required)
{
    if (!value || value[0] == '\0') {
        if (required) {
            ESP_LOGE(TAG, "%s is required", name);
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }

    if (!mqtt_string_fits(value, capacity)) {
        ESP_LOGE(TAG, "%s is too long (maximum %u bytes)",
                 name, (unsigned)(capacity - 1));
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t store_optional_nvs_string(nvs_handle_t nvs,
                                           const char *key,
                                           const char *value)
{
    if (value && value[0] != '\0') {
        return nvs_set_str(nvs, key, value);
    }

    esp_err_t err = nvs_erase_key(nvs, key);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
}

/* ── Public API ─────────────────────────────────────────────── */

esp_err_t mqtt_bot_init(void)
{
    /* Start with build-time secrets as defaults */
    load_default_string("broker URI", MIMI_SECRET_MQTT_URI,
                        s_broker_uri, sizeof(s_broker_uri));
    load_default_string("client ID", MIMI_SECRET_MQTT_CLIENT_ID,
                        s_client_id, sizeof(s_client_id));
    load_default_string("username", MIMI_SECRET_MQTT_USERNAME,
                        s_username, sizeof(s_username));
    load_default_string("password", MIMI_SECRET_MQTT_PASSWORD,
                        s_password, sizeof(s_password));
    load_default_string("subscribe topic", MIMI_MQTT_DEFAULT_SUB_TOPIC,
                        s_subscribe_topic, sizeof(s_subscribe_topic));

    /* Load configuration from NVS (overrides build-time) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_MQTT, NVS_READWRITE, &nvs) == ESP_OK) {
        load_nvs_string(nvs, MIMI_NVS_KEY_MQTT_URI,
                        s_broker_uri, sizeof(s_broker_uri));
        load_nvs_string(nvs, MIMI_NVS_KEY_MQTT_CLIENT_ID,
                        s_client_id, sizeof(s_client_id));
        load_nvs_string(nvs, MIMI_NVS_KEY_MQTT_USERNAME,
                        s_username, sizeof(s_username));
        load_nvs_string(nvs, MIMI_NVS_KEY_MQTT_PASSWORD,
                        s_password, sizeof(s_password));
        load_nvs_string(nvs, MIMI_NVS_KEY_MQTT_SUB_TOPIC,
                        s_subscribe_topic, sizeof(s_subscribe_topic));

        nvs_close(nvs);
    }

    /* Check if MQTT is configured */
    s_enabled = (s_broker_uri[0] != '\0');
    if (s_enabled) {
        ESP_LOGI(TAG, "MQTT configured: %s (client_id=%s)",
                 s_broker_uri,
                 s_client_id[0] ? s_client_id : "(auto)");
    } else {
        ESP_LOGW(TAG, "No MQTT broker configured. Use CLI: set_mqtt_config <uri> [client_id] [username] [password]");
    }

    return ESP_OK;
}

esp_err_t mqtt_bot_start(void)
{
    if (!s_enabled) {
        ESP_LOGW(TAG, "MQTT not configured, skipping start");
        return ESP_OK;
    }

    if (s_mqtt_client != NULL) {
        ESP_LOGW(TAG, "MQTT client already running");
        return ESP_OK;
    }

    /* Generate client ID if not set */
    char client_id[80] = {0};
    if (s_client_id[0] == '\0') {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        snprintf(client_id, sizeof(client_id), "mimiclaw_%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        mqtt_copy_string(client_id, sizeof(client_id), s_client_id);
    }

    /* Configure MQTT client */
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_broker_uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials.client_id = client_id,
        .credentials.username = s_username[0] ? s_username : NULL,
        .credentials.authentication.password = s_password[0] ? s_password : NULL,
        .session.keepalive = MIMI_MQTT_KEEPALIVE_S,
        .network.timeout_ms = MIMI_MQTT_TIMEOUT_MS,
        .network.refresh_connection_after_ms = 0,
        .session.disable_clean_session = false,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_FAIL;
    }

    /* Register event handler */
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    /* Start the client */
    esp_err_t err = esp_mqtt_client_start(s_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_mqtt_client);
        s_mqtt_client = NULL;
        return err;
    }

    ESP_LOGI(TAG, "MQTT client started");
    return ESP_OK;
}

esp_err_t mqtt_bot_stop(void)
{
    if (s_mqtt_client == NULL) {
        mqtt_message_assembly_reset(&s_inbound_assembly);
        return ESP_OK;
    }

    esp_err_t err = esp_mqtt_client_stop(s_mqtt_client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Error stopping MQTT client: %s", esp_err_to_name(err));
    }

    esp_mqtt_client_destroy(s_mqtt_client);
    s_mqtt_client = NULL;
    s_connected = false;
    mqtt_message_assembly_reset(&s_inbound_assembly);

    ESP_LOGI(TAG, "MQTT client stopped");
    return ESP_OK;
}

esp_err_t mqtt_send_message(const char *topic, const char *text)
{
    if (!topic || topic[0] == '\0' || !text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_enabled || s_mqtt_client == NULL) {
        ESP_LOGW(TAG, "Cannot send: MQTT not configured or not started");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_connected) {
        ESP_LOGW(TAG, "Cannot send: MQTT not connected");
        return ESP_ERR_INVALID_STATE;
    }

    /* Convert request topic to response topic */
    char response_topic[256];
    if (!mqtt_build_response_topic(topic, response_topic,
                                   sizeof(response_topic))) {
        ESP_LOGW(TAG, "Invalid or oversized MQTT publish topic");
        return ESP_ERR_INVALID_ARG;
    }

    /* Split long messages if needed */
    size_t text_len = strlen(text);
    size_t offset = 0;
    int all_ok = 1;

    while (offset < text_len) {
        size_t chunk = text_len - offset;
        if (chunk > MIMI_MQTT_MAX_MSG_LEN) {
            chunk = MIMI_MQTT_MAX_MSG_LEN;
        }

        /* Create chunk with null terminator */
        char *segment = malloc(chunk + 1);
        if (!segment) return ESP_ERR_NO_MEM;
        memcpy(segment, text + offset, chunk);
        segment[chunk] = '\0';

        /* Build JSON payload */
        cJSON *payload = cJSON_CreateObject();
        if (!payload ||
            !cJSON_AddStringToObject(payload, "text", segment) ||
            !cJSON_AddStringToObject(payload, "source", "mimiclaw")) {
            cJSON_Delete(payload);
            free(segment);
            return ESP_ERR_NO_MEM;
        }
        char *json_str = cJSON_PrintUnformatted(payload);
        cJSON_Delete(payload);
        free(segment);

        if (!json_str) {
            offset += chunk;
            all_ok = 0;
            continue;
        }

        /* Publish message */
        int msg_id = esp_mqtt_client_publish(s_mqtt_client, response_topic, json_str, 0, 1, 0);
        free(json_str);

        if (msg_id < 0) {
            ESP_LOGE(TAG, "Failed to publish to %s", response_topic);
            all_ok = 0;
        } else {
            ESP_LOGI(TAG, "Published to %s (%d bytes, msg_id=%d)", response_topic, (int)chunk, msg_id);
        }

        offset += chunk;
    }

    return all_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t mqtt_set_config(const char *broker_uri, const char *client_id,
                          const char *username, const char *password)
{
    esp_err_t err = validate_mqtt_value(
        "Broker URI", broker_uri, sizeof(s_broker_uri), true);
    if (err == ESP_OK) {
        err = validate_mqtt_value(
            "Client ID", client_id, sizeof(s_client_id), false);
    }
    if (err == ESP_OK) {
        err = validate_mqtt_value(
            "Username", username, sizeof(s_username), false);
    }
    if (err == ESP_OK) {
        err = validate_mqtt_value(
            "Password", password, sizeof(s_password), false);
    }
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_open(MIMI_NVS_MQTT, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs, MIMI_NVS_KEY_MQTT_URI, broker_uri);
    if (err == ESP_OK) {
        err = store_optional_nvs_string(
            nvs, MIMI_NVS_KEY_MQTT_CLIENT_ID, client_id);
    }
    if (err == ESP_OK) {
        err = store_optional_nvs_string(
            nvs, MIMI_NVS_KEY_MQTT_USERNAME, username);
    }
    if (err == ESP_OK) {
        err = store_optional_nvs_string(
            nvs, MIMI_NVS_KEY_MQTT_PASSWORD, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save MQTT config: %s",
                 esp_err_to_name(err));
        return err;
    }

    mqtt_copy_string(s_broker_uri, sizeof(s_broker_uri), broker_uri);
    if (client_id && client_id[0]) {
        mqtt_copy_string(s_client_id, sizeof(s_client_id), client_id);
    } else {
        s_client_id[0] = '\0';
    }
    if (username && username[0]) {
        mqtt_copy_string(s_username, sizeof(s_username), username);
    } else {
        s_username[0] = '\0';
    }
    if (password && password[0]) {
        mqtt_copy_string(s_password, sizeof(s_password), password);
    } else {
        s_password[0] = '\0';
    }

    s_enabled = (s_broker_uri[0] != '\0');
    ESP_LOGI(TAG, "MQTT config saved: %s", s_broker_uri);

    return ESP_OK;
}

esp_err_t mqtt_set_subscribe_topic(const char *topic_pattern)
{
    if (validate_mqtt_value(
            "Subscribe topic", topic_pattern,
            sizeof(s_subscribe_topic), true) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_MQTT, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, MIMI_NVS_KEY_MQTT_SUB_TOPIC, topic_pattern);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save MQTT subscribe topic: %s",
                 esp_err_to_name(err));
        return err;
    }

    mqtt_copy_string(s_subscribe_topic, sizeof(s_subscribe_topic),
                     topic_pattern);
    ESP_LOGI(TAG, "Subscribe topic set: %s", s_subscribe_topic);

    /* Re-subscribe if already connected */
    if (s_connected && s_mqtt_client) {
        esp_mqtt_client_subscribe(s_mqtt_client, s_subscribe_topic, 1);
    }

    return ESP_OK;
}

bool mqtt_is_connected(void)
{
    return s_connected;
}
