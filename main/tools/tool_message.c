#include "tools/tool_message.h"
#include "bus/message_bus.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_message";

/**
 * Validate that channel is one of the supported channels.
 */
static bool validate_channel(const char *channel)
{
    if (!channel) return false;
    return (strcmp(channel, MIMI_CHAN_TELEGRAM) == 0 ||
            strcmp(channel, MIMI_CHAN_WEBSOCKET) == 0 ||
            strcmp(channel, MIMI_CHAN_FEISHU) == 0);
}

esp_err_t tool_message_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *channel = cJSON_GetStringValue(cJSON_GetObjectItem(root, "channel"));
    const char *chat_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "chat_id"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));

    if (!channel || channel[0] == '\0') {
        snprintf(output, output_size, "Error: missing or empty 'channel' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (!validate_channel(channel)) {
        snprintf(output, output_size,
                 "Error: invalid channel '%s'. Must be one of: telegram, websocket, feishu",
                 channel);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (!chat_id || chat_id[0] == '\0') {
        snprintf(output, output_size, "Error: missing or empty 'chat_id' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (!text || text[0] == '\0') {
        snprintf(output, output_size, "Error: missing or empty 'text' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Construct the outbound message */
    mimi_msg_t msg = {0};
    strncpy(msg.channel, channel, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
    msg.content = strdup(text);

    if (!msg.content) {
        snprintf(output, output_size, "Error: out of memory");
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    /* Push to outbound queue — bus takes ownership of content */
    esp_err_t err = message_bus_push_outbound(&msg);
    if (err != ESP_OK) {
        free(msg.content);
        snprintf(output, output_size, "Error: failed to push message to outbound queue");
        cJSON_Delete(root);
        return err;
    }

    snprintf(output, output_size,
             "OK: message sent to %s chat_id=%s (%d bytes)",
             channel, chat_id, (int)strlen(text));
    ESP_LOGI(TAG, "send_message: %s/%s (%d bytes)", channel, chat_id, (int)strlen(text));

    cJSON_Delete(root);
    return ESP_OK;
}
