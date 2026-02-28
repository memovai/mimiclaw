#include "message_bus.h"
#include "mimi_config.h"
#include "esp_log.h"
#include <string.h>
#include <stdbool.h>

static const char *TAG = "bus";

static QueueHandle_t s_inbound_queue;
static QueueHandle_t s_outbound_queue;
static char s_latest_channel[16];
static char s_latest_chat_id[32];
static bool s_has_latest_context = false;
static portMUX_TYPE s_ctx_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t message_bus_init(void)
{
    s_inbound_queue = xQueueCreate(MIMI_BUS_QUEUE_LEN, sizeof(mimi_msg_t));
    s_outbound_queue = xQueueCreate(MIMI_BUS_QUEUE_LEN, sizeof(mimi_msg_t));

    if (!s_inbound_queue || !s_outbound_queue) {
        ESP_LOGE(TAG, "Failed to create message queues");
        return ESP_ERR_NO_MEM;
    }

    memset(s_latest_channel, 0, sizeof(s_latest_channel));
    memset(s_latest_chat_id, 0, sizeof(s_latest_chat_id));
    s_has_latest_context = false;

    ESP_LOGI(TAG, "Message bus initialized (queue depth %d)", MIMI_BUS_QUEUE_LEN);
    return ESP_OK;
}

esp_err_t message_bus_push_inbound(const mimi_msg_t *msg)
{
    if (xQueueSend(s_inbound_queue, msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Inbound queue full, dropping message");
        return ESP_ERR_NO_MEM;
    }

    if (strcmp(msg->channel, MIMI_CHAN_SYSTEM) != 0 && msg->chat_id[0] != '\0') {
        portENTER_CRITICAL(&s_ctx_lock);
        strncpy(s_latest_channel, msg->channel, sizeof(s_latest_channel) - 1);
        s_latest_channel[sizeof(s_latest_channel) - 1] = '\0';
        strncpy(s_latest_chat_id, msg->chat_id, sizeof(s_latest_chat_id) - 1);
        s_latest_chat_id[sizeof(s_latest_chat_id) - 1] = '\0';
        s_has_latest_context = true;
        portEXIT_CRITICAL(&s_ctx_lock);
    }

    return ESP_OK;
}

esp_err_t message_bus_pop_inbound(mimi_msg_t *msg, uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_inbound_queue, msg, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t message_bus_push_outbound(const mimi_msg_t *msg)
{
    if (xQueueSend(s_outbound_queue, msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Outbound queue full, dropping message");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t message_bus_pop_outbound(mimi_msg_t *msg, uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_outbound_queue, msg, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t message_bus_get_latest_client_context(char *channel, size_t channel_size,
                                                char *chat_id, size_t chat_id_size)
{
    if (!channel || channel_size == 0 || !chat_id || chat_id_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_ctx_lock);
    if (!s_has_latest_context) {
        portEXIT_CRITICAL(&s_ctx_lock);
        return ESP_ERR_NOT_FOUND;
    }

    strncpy(channel, s_latest_channel, channel_size - 1);
    channel[channel_size - 1] = '\0';
    strncpy(chat_id, s_latest_chat_id, chat_id_size - 1);
    chat_id[chat_id_size - 1] = '\0';
    portEXIT_CRITICAL(&s_ctx_lock);

    return ESP_OK;
}
