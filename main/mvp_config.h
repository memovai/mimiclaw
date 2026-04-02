#pragma once

/*
 * MimiClaw LLM MVP configuration.
 *
 * 为了方便学习和改动，这个版本把所有关键输入都直接放在代码里：
 * - WiFi 名称和密码
 * - DeepSeek API Key
 * - 固定 system prompt
 * - 固定 user question
 */

/* WiFi */
#define MVP_WIFI_SSID               "CMCC-TeYM"
#define MVP_WIFI_PASS               "d245g4jb"
#define MVP_WIFI_MAX_RETRY          10
#define MVP_WIFI_RETRY_BASE_MS      1000
#define MVP_WIFI_RETRY_MAX_MS       30000
#define MVP_WIFI_CONNECT_TIMEOUT_MS 30000

/* DeepSeek OpenAI-compatible chat endpoint */
#define MVP_LLM_PROVIDER            "deepseek"
#define MVP_LLM_BASE_URL            "https://api.deepseek.com"
#define MVP_LLM_CHAT_PATH           "/chat/completions"
#define MVP_LLM_API_KEY             "sk-c5d676e140c74b5b9a9219353f3eae82"
#define MVP_LLM_MODEL               "deepseek-chat"
#define MVP_LLM_TIMEOUT_MS          120000
#define MVP_LLM_MAX_TOKENS          1024
#define MVP_LLM_RESPONSE_BUF_SIZE   (24 * 1024)
#define MVP_LLM_PREVIEW_BYTES       200

/* Prompt */
#define MVP_SYSTEM_PROMPT           "You are a helpful assistant running on an ESP32-S3 device. Answer clearly and briefly in Chinese unless the user asks otherwise."
#define MVP_USER_QUESTION           "你喜欢猫还是喜欢狗？"

/* Runtime behavior */
#define MVP_POST_RESULT_DELAY_MS    10000
