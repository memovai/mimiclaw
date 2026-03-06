#pragma once

#include "esp_err.h"

/**
 * Initialize the Feishu bot (load credentials from NVS / build-time).
 */
esp_err_t feishu_bot_init(void);

/**
 * Start the Feishu webhook HTTP server for receiving events.
 * Listens on MIMI_FEISHU_WEBHOOK_PORT.
 */
esp_err_t feishu_bot_start(void);

/**
 * Send a text message to a Feishu chat.
 * Automatically splits messages longer than MIMI_FEISHU_MAX_MSG_LEN chars.
 * @param chat_id  Feishu chat ID (open_id or chat_id)
 * @param text     Message text
 */
esp_err_t feishu_send_message(const char *chat_id, const char *text);

/**
 * Reply to a specific message in a Feishu chat.
 * @param message_id  The message_id to reply to
 * @param text        Reply text
 */
esp_err_t feishu_reply_message(const char *message_id, const char *text);

/**
 * Save Feishu app credentials to NVS.
 * @param app_id     Feishu App ID
 * @param app_secret Feishu App Secret
 */
esp_err_t feishu_set_credentials(const char *app_id, const char *app_secret);
