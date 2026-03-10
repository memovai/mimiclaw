#pragma once

#include "esp_err.h"

/**
 * Initialize the iMessage bot (loads credentials from NVS / build-time).
 */
esp_err_t imessage_bot_init(void);

/**
 * Start the iMessage polling task (HTTP polling on Core 0).
 */
esp_err_t imessage_bot_start(void);

/**
 * Send a text message via iMessage.
 * Automatically splits messages longer than 4096 chars.
 * @param chat_id  iMessage address (email or +phone) or "group:<id>"
 * @param text     Message text
 */
esp_err_t imessage_send_message(const char *chat_id, const char *text);

/**
 * Save iMessage proxy credentials to NVS.
 * @param server_url  Upstream iMessage Kit server URL
 * @param api_key     API key
 * @param proxy_url   REST proxy base URL, or NULL to keep current
 */
esp_err_t imessage_set_credentials(const char *server_url, const char *api_key,
                                   const char *proxy_url);
