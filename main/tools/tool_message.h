#pragma once
#include "esp_err.h"
#include <stddef.h>

/**
 * Send a message to a specific channel and chat.
 * Input JSON: {"channel": "telegram|websocket|feishu", "chat_id": "...", "text": "..."}
 */
esp_err_t tool_message_execute(const char *input_json, char *output, size_t output_size);
