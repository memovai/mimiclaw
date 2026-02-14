#pragma once

#include "esp_err.h"

/**
 * Initialize the Feishu bot.
 */
esp_err_t feishu_bot_init(void);

/**
 * Start the Feishu polling task (long polling on Core 0).
 */
esp_err_t feishu_bot_start(void);

/**
 * Send a text message to a Feishu chat.
 * Automatically splits messages longer than 4096 chars.
 * @param chat_id  Feishu chat ID (string)
 * @param text     Message text
 */
esp_err_t feishu_send_message(const char *chat_id, const char *text);

/**
 * Save the Feishu app credentials to NVS.
 * @param app_id     Feishu App ID
 * @param app_secret Feishu App Secret
 */
esp_err_t feishu_set_credentials(const char *app_id, const char *app_secret);
