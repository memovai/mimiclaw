#pragma once

/* MimiClaw Global Configuration */

/* Build-time secrets (highest priority, override NVS) */
#if __has_include("mimi_secrets.h")
#include "mimi_secrets.h"
#endif

#ifndef MIMI_SECRET_WIFI_SSID
#define MIMI_SECRET_WIFI_SSID       ""
#endif
#ifndef MIMI_SECRET_WIFI_PASS
#define MIMI_SECRET_WIFI_PASS       ""
#endif
#ifndef MIMI_SECRET_TG_TOKEN
#define MIMI_SECRET_TG_TOKEN        ""
#endif
#ifndef MIMI_SECRET_API_KEY
#define MIMI_SECRET_API_KEY         ""
#endif
#ifndef MIMI_SECRET_LLM_API_URL
#define MIMI_SECRET_LLM_API_URL     ""
#endif
#ifndef MIMI_SECRET_MODEL
#define MIMI_SECRET_MODEL           ""
#endif
#ifndef MIMI_SECRET_MODEL_PROVIDER
#define MIMI_SECRET_MODEL_PROVIDER  "anthropic"
#endif
#ifndef MIMI_SECRET_PROXY_HOST
#define MIMI_SECRET_PROXY_HOST      ""
#endif
#ifndef MIMI_SECRET_PROXY_PORT
#define MIMI_SECRET_PROXY_PORT      ""
#endif
#ifndef MIMI_SECRET_PROXY_TYPE
#define MIMI_SECRET_PROXY_TYPE      ""
#endif
#ifndef MIMI_SECRET_SEARCH_KEY
#define MIMI_SECRET_SEARCH_KEY      ""
#endif
#ifndef MIMI_SECRET_FEISHU_APP_ID
#define MIMI_SECRET_FEISHU_APP_ID   ""
#endif
#ifndef MIMI_SECRET_FEISHU_APP_SECRET
#define MIMI_SECRET_FEISHU_APP_SECRET ""
#endif
#ifndef MIMI_SECRET_TAVILY_KEY
#define MIMI_SECRET_TAVILY_KEY      ""
#endif
#ifndef MIMI_SECRET_STT_URL
#define MIMI_SECRET_STT_URL         ""
#endif
#ifndef MIMI_SECRET_STT_API_KEY
#define MIMI_SECRET_STT_API_KEY     ""
#endif
#ifndef MIMI_SECRET_STT_MODEL
#define MIMI_SECRET_STT_MODEL       ""
#endif
#ifndef MIMI_SECRET_TTS_URL
#define MIMI_SECRET_TTS_URL         ""
#endif
#ifndef MIMI_SECRET_TTS_API_KEY
#define MIMI_SECRET_TTS_API_KEY     ""
#endif
#ifndef MIMI_SECRET_TTS_VOICE
#define MIMI_SECRET_TTS_VOICE       "Cherry"
#endif
#ifndef MIMI_SECRET_TTS_MODEL
#define MIMI_SECRET_TTS_MODEL       ""
#endif
#ifndef MIMI_SECRET_TTS_LANGUAGE
#define MIMI_SECRET_TTS_LANGUAGE    "English"
#endif

/* Qwen voice API defaults (DashScope) */
#define MIMI_QWEN_STT_URL           "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
#define MIMI_QWEN_STT_MODEL         "qwen3-asr-flash"
#define MIMI_QWEN_TTS_URL           "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"
#define MIMI_QWEN_TTS_MODEL         "qwen3-tts-flash"

/* WiFi */
#define MIMI_WIFI_MAX_RETRY          10
#define MIMI_WIFI_RETRY_BASE_MS      1000
#define MIMI_WIFI_RETRY_MAX_MS       30000

/* Telegram Bot */
#define MIMI_TG_POLL_TIMEOUT_S       30
#define MIMI_TG_MAX_MSG_LEN          4096
#define MIMI_TG_POLL_STACK           (12 * 1024)
#define MIMI_TG_POLL_PRIO            5
#define MIMI_TG_POLL_CORE            0
#define MIMI_TG_CARD_SHOW_MS         3000
#define MIMI_TG_CARD_BODY_SCALE      3

/* Feishu Bot */
#define MIMI_FEISHU_MAX_MSG_LEN          4096
#define MIMI_FEISHU_POLL_STACK           (12 * 1024)
#define MIMI_FEISHU_POLL_PRIO            5
#define MIMI_FEISHU_POLL_CORE            0
#define MIMI_FEISHU_WEBHOOK_PORT         18790
#define MIMI_FEISHU_WEBHOOK_PATH         "/feishu/events"
#define MIMI_FEISHU_WEBHOOK_MAX_BODY     (16 * 1024)

/* Agent Loop */
#define MIMI_AGENT_STACK             (24 * 1024)
#define MIMI_AGENT_PRIO              6
#define MIMI_AGENT_CORE              1
#define MIMI_AGENT_MAX_HISTORY       20
#define MIMI_AGENT_MAX_TOOL_ITER     10
#define MIMI_MAX_TOOL_CALLS          4
#define MIMI_AGENT_SEND_WORKING_STATUS 1

/* Voice UX (LLM -> TTS) */
/* Rough speaking rate for Simplified Chinese TTS is often ~4–6 chars/sec depending on voice.
    * Default limits aim to keep playback under ~20 seconds in typical conditions.
    * Override these in mimi_secrets.h per your preferred voice/speed.
    */
#ifndef MIMI_VOICE_TTS_MAX_SECONDS
#define MIMI_VOICE_TTS_MAX_SECONDS  20
#endif

#ifndef MIMI_VOICE_TTS_CHARS_PER_SEC
#define MIMI_VOICE_TTS_CHARS_PER_SEC 7
#endif

#ifndef MIMI_VOICE_LLM_MAX_CHARS
#define MIMI_VOICE_LLM_MAX_CHARS     (MIMI_VOICE_TTS_MAX_SECONDS * MIMI_VOICE_TTS_CHARS_PER_SEC)
#endif

#ifndef MIMI_VOICE_TTS_MAX_CHARS
#define MIMI_VOICE_TTS_MAX_CHARS     (MIMI_VOICE_LLM_MAX_CHARS + 10)
#endif

/* Voice capture (VAD / STT trigger) */
#ifndef MIMI_VOICE_VAD_START_FRAMES
#define MIMI_VOICE_VAD_START_FRAMES  4   /* consecutive frames above threshold to enter speech */
#endif

#ifndef MIMI_VOICE_VAD_MIN_FRAMES
#define MIMI_VOICE_VAD_MIN_FRAMES    50  /* minimum utterance frames before sending to STT */
#endif

#ifndef MIMI_VOICE_STT_COOLDOWN_MS
#define MIMI_VOICE_STT_COOLDOWN_MS   2000 /* cooldown after an STT attempt to reduce re-trigger */
#endif

/* Timezone (POSIX TZ format) */
#define MIMI_TIMEZONE                "PST8PDT,M3.2.0,M11.1.0"

/* LLM */
#define MIMI_LLM_DEFAULT_MODEL       "claude-opus-4-5"
#define MIMI_LLM_PROVIDER_DEFAULT    "anthropic"
#define MIMI_LLM_MAX_TOKENS          4096
#define MIMI_LLM_API_BASE_ANTHROPIC  "https://api.anthropic.com/v1"
#define MIMI_LLM_API_BASE_OPENAI     "https://api.openai.com/v1"
#define MIMI_LLM_API_VERSION         "2023-06-01"
#define MIMI_LLM_STREAM_BUF_SIZE     (32 * 1024)
#define MIMI_LLM_LOG_VERBOSE_PAYLOAD 0
#define MIMI_LLM_LOG_PREVIEW_BYTES   160

/* Message Bus */
#define MIMI_BUS_QUEUE_LEN           16
#define MIMI_OUTBOUND_STACK          (12 * 1024)
#define MIMI_OUTBOUND_PRIO           5
#define MIMI_OUTBOUND_CORE           0

/* Voice speak task (TTS download + resample + playback) */
#ifndef MIMI_VOICE_SPEAK_STACK
#define MIMI_VOICE_SPEAK_STACK       (12 * 1024)
#endif
#ifndef MIMI_VOICE_SPEAK_PRIO
#define MIMI_VOICE_SPEAK_PRIO        5
#endif
#ifndef MIMI_VOICE_SPEAK_CORE
#define MIMI_VOICE_SPEAK_CORE        1
#endif

/* WiFi reliability */
#ifndef MIMI_WIFI_DISABLE_POWERSAVE
#define MIMI_WIFI_DISABLE_POWERSAVE  1
#endif

/* Memory / SPIFFS */
#define MIMI_SPIFFS_BASE             "/spiffs"
#define MIMI_SPIFFS_CONFIG_DIR       MIMI_SPIFFS_BASE "/config"
#define MIMI_SPIFFS_MEMORY_DIR       MIMI_SPIFFS_BASE "/memory"
#define MIMI_SPIFFS_SESSION_DIR      MIMI_SPIFFS_BASE "/sessions"
#define MIMI_MEMORY_FILE             MIMI_SPIFFS_MEMORY_DIR "/MEMORY.md"
#define MIMI_SOUL_FILE               MIMI_SPIFFS_CONFIG_DIR "/SOUL.md"
#define MIMI_USER_FILE               MIMI_SPIFFS_CONFIG_DIR "/USER.md"
#define MIMI_CONTEXT_BUF_SIZE        (16 * 1024)
#define MIMI_SESSION_MAX_MSGS        20

/* Cron / Heartbeat */
#define MIMI_CRON_FILE               MIMI_SPIFFS_BASE "/cron.json"
#define MIMI_CRON_MAX_JOBS           16
#define MIMI_CRON_CHECK_INTERVAL_MS  (60 * 1000)
#define MIMI_HEARTBEAT_FILE          MIMI_SPIFFS_BASE "/HEARTBEAT.md"
#define MIMI_HEARTBEAT_INTERVAL_MS   (30 * 60 * 1000)

/* Skills */
#define MIMI_SKILLS_PREFIX           MIMI_SPIFFS_BASE "/skills/"

/* WebSocket Gateway */
#define MIMI_WS_PORT                 18789
#define MIMI_WS_MAX_CLIENTS          4

/* Serial CLI */
#define MIMI_CLI_STACK               (4 * 1024)
#define MIMI_CLI_PRIO                3
#define MIMI_CLI_CORE                0

/* NVS Namespaces */
#define MIMI_NVS_WIFI                "wifi_config"
#define MIMI_NVS_TG                  "tg_config"
#define MIMI_NVS_FEISHU              "feishu_config"
#define MIMI_NVS_LLM                 "llm_config"
#define MIMI_NVS_PROXY               "proxy_config"
#define MIMI_NVS_SEARCH              "search_config"

/* NVS Keys */
#define MIMI_NVS_KEY_SSID            "ssid"
#define MIMI_NVS_KEY_PASS            "password"
#define MIMI_NVS_KEY_TG_TOKEN        "bot_token"
#define MIMI_NVS_KEY_FEISHU_APP_ID   "app_id"
#define MIMI_NVS_KEY_FEISHU_APP_SECRET "app_secret"
#define MIMI_NVS_KEY_API_KEY         "api_key"
#define MIMI_NVS_KEY_API_BASE        "api_base"
#define MIMI_NVS_KEY_TAVILY_KEY      "tavily_key"
#define MIMI_NVS_KEY_MODEL           "model"
#define MIMI_NVS_KEY_PROVIDER        "provider"
#define MIMI_NVS_KEY_PROXY_HOST      "host"
#define MIMI_NVS_KEY_PROXY_PORT      "port"
