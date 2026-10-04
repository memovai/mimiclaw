#pragma once

#include "esp_err.h"

/**
 * Initialize the Telegram bot.
 */
esp_err_t telegram_bot_init(void);

/**
 * Start the Telegram polling task (long polling on Core 0).
 */
esp_err_t telegram_bot_start(void);

/**
 * Send a text message to a Telegram chat.
 * Automatically splits messages longer than 4096 chars.
 * @param chat_id  Telegram chat ID (numeric string)
 * @param text     Message text (supports Markdown)
 */
esp_err_t telegram_send_message(const char *chat_id, const char *text);

/**
 * Save the Telegram bot token to NVS.
 */
esp_err_t telegram_set_token(const char *token);

/**
 * Send a one-time "device booted" notification to the admin chat.
 *
 * No-op when MIMI_TG_SEND_FIRST_BOOT is 0, MIMI_SECRET_TG_ADMIN_CHAT_ID
 * is empty, or the NVS flag MIMI_NVS_KEY_FIRST_BOOT_DONE is already set.
 * On a successful send the flag is written so subsequent boots are silent.
 * If sending fails the flag is left unset so the next boot can retry.
 * Always returns ESP_OK so callers can ignore the result on boot path.
 */
esp_err_t telegram_send_first_boot_notice(void);

/**
 * Clear the first-boot-done NVS flag so the next boot re-sends the
 * notification (useful for testing without erasing the whole NVS).
 */
esp_err_t telegram_reset_first_boot_flag(void);

