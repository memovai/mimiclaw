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
#ifndef MIMI_SECRET_MODEL
#define MIMI_SECRET_MODEL           ""
#endif
#ifndef MIMI_SECRET_MODEL_PROVIDER
#define MIMI_SECRET_MODEL_PROVIDER  "openai"
#endif
#ifndef MIMI_SECRET_PROXY_HOST
#define MIMI_SECRET_PROXY_HOST      ""
#endif
#ifndef MIMI_SECRET_PROXY_PORT
#define MIMI_SECRET_PROXY_PORT      ""
#endif
#ifndef MIMI_SECRET_SEARCH_KEY
#define MIMI_SECRET_SEARCH_KEY      ""
#endif
#ifndef MIMI_SECRET_STT_URL
#define MIMI_SECRET_STT_URL         "https://api.openai.com/v1/audio/transcriptions"
#endif
#ifndef MIMI_SECRET_STT_HOST
#define MIMI_SECRET_STT_HOST        "192.168.1.100"
#endif
#ifndef MIMI_SECRET_STT_PORT
#define MIMI_SECRET_STT_PORT        "8000"
#endif
#ifndef MIMI_SECRET_STT_MODEL
#define MIMI_SECRET_STT_MODEL       "gpt-4o-mini-transcribe"
#endif
#ifndef MIMI_SECRET_TTS_URL
#define MIMI_SECRET_TTS_URL         "https://api.openai.com/v1/audio/speech"
#endif
#ifndef MIMI_SECRET_TTS_MODEL
#define MIMI_SECRET_TTS_MODEL       "gpt-4o-mini-tts"
#endif
#ifndef MIMI_SECRET_TTS_VOICE
#define MIMI_SECRET_TTS_VOICE       "nova"
#endif
#ifndef MIMI_SECRET_TTS_INSTRUCTIONS
#define MIMI_SECRET_TTS_INSTRUCTIONS ""
#endif
#ifndef MIMI_SECRET_API_URL
#define MIMI_SECRET_API_URL         ""
#endif

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

/* Agent Loop */
#define MIMI_AGENT_STACK             (12 * 1024)
#define MIMI_AGENT_PRIO              6
#define MIMI_AGENT_CORE              1
#define MIMI_AGENT_MAX_HISTORY       20
#define MIMI_AGENT_MAX_TOOL_ITER     10
#define MIMI_MAX_TOOL_CALLS          4
#define MIMI_ENABLE_TOOLS            0        /* 0 = disable tools, 1 = enable */

/* Timezone (POSIX TZ format) */
#define MIMI_TIMEZONE                "PST8PDT,M3.2.0,M11.1.0"

/* LLM */
#define MIMI_LLM_DEFAULT_MODEL       "gpt-4o-mini"
#define MIMI_LLM_PROVIDER_DEFAULT    "openai"
#define MIMI_LLM_MAX_TOKENS          4096
#define MIMI_LLM_API_URL             "https://api.anthropic.com/v1/messages"
#define MIMI_OPENAI_API_URL          "https://api.openai.com/v1/chat/completions"
#define MIMI_LLM_API_VERSION         "2023-06-01"
#define MIMI_LLM_STREAM_BUF_SIZE     (32 * 1024)

/* Message Bus */
#define MIMI_BUS_QUEUE_LEN           8
#define MIMI_OUTBOUND_STACK          (8 * 1024)
#define MIMI_OUTBOUND_PRIO           5
#define MIMI_OUTBOUND_CORE           0

/* Memory / SPIFFS */
#define MIMI_SPIFFS_BASE             "/spiffs"
#define MIMI_SPIFFS_CONFIG_DIR       "/spiffs/config"
#define MIMI_SPIFFS_MEMORY_DIR       "/spiffs/memory"
#define MIMI_SPIFFS_SESSION_DIR      "/spiffs/sessions"
#define MIMI_MEMORY_FILE             "/spiffs/memory/MEMORY.md"
#define MIMI_SOUL_FILE               "/spiffs/config/SOUL.md"
#define MIMI_USER_FILE               "/spiffs/config/USER.md"
#define MIMI_CONTEXT_BUF_SIZE        (16 * 1024)
#define MIMI_SESSION_MAX_MSGS        20

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
#define MIMI_NVS_LLM                 "llm_config"
#define MIMI_NVS_PROXY               "proxy_config"
#define MIMI_NVS_SEARCH              "search_config"

/* ─── Hardware Pin Assignments (xingzhi-cube 1.83" 2mic board) ─── */

/* I2C Bus (shared: ES8311 DAC + ES7210 ADC) */
#define MIMI_I2C_NUM             I2C_NUM_0
#define MIMI_I2C_SDA             GPIO_NUM_12
#define MIMI_I2C_SCL             GPIO_NUM_11
#define MIMI_I2C_FREQ_HZ         400000

/* I2S Bus (shared: speaker TX + mic RX) */
#define MIMI_I2S_NUM             I2S_NUM_0
#define MIMI_I2S_MCLK            GPIO_NUM_5
#define MIMI_I2S_BCLK            GPIO_NUM_15
#define MIMI_I2S_LRCLK           GPIO_NUM_16
#define MIMI_I2S_DOUT            GPIO_NUM_6     /* ESP32 → ES8311 speaker DAC */
#define MIMI_I2S_DIN             GPIO_NUM_7     /* ES7210 mic ADC → ESP32 */

/* Speaker */
#define MIMI_SPEAKER_EN          GPIO_NUM_4     /* PA enable, active high */

/* Audio parameters */
#define MIMI_AUDIO_SAMPLE_RATE   16000
#define MIMI_AUDIO_BITS          16
#define MIMI_AUDIO_CHANNELS      1              /* mono for STT/TTS */

/* I2S DMA */
#define MIMI_I2S_DMA_DESC_NUM    6
#define MIMI_I2S_DMA_FRAME_NUM   240

/* Display (1.83" SPI LCD, ST7789V, 284x240) */
#define MIMI_LCD_SPI_HOST        SPI2_HOST
#define MIMI_LCD_CLK             GPIO_NUM_9
#define MIMI_LCD_MOSI            GPIO_NUM_10
#define MIMI_LCD_CS              GPIO_NUM_14
#define MIMI_LCD_DC              GPIO_NUM_8
#define MIMI_LCD_RST             GPIO_NUM_18
#define MIMI_LCD_BL              GPIO_NUM_13
#define MIMI_LCD_WIDTH           284
#define MIMI_LCD_HEIGHT          240

/* Buttons */
#define MIMI_BTN_WAKE            GPIO_NUM_0     /* Top: wake / push-to-talk */
#define MIMI_BTN_MUTE            GPIO_NUM_39    /* Left: mute toggle */
#define MIMI_BTN_VOLUME          GPIO_NUM_40    /* Right: volume cycle */

/* WS2812 LED */
#define MIMI_LED_PIN             GPIO_NUM_48

/* Voice Channel */
#define MIMI_VOICE_STACK             (8 * 1024)
#define MIMI_VOICE_PRIO              5
#define MIMI_VOICE_CORE              0
#define MIMI_VOICE_MAX_RECORDING_S   10
#define MIMI_VOICE_RECORDING_BUF_SIZE (MIMI_AUDIO_SAMPLE_RATE * 2 * MIMI_VOICE_MAX_RECORDING_S)
#define MIMI_VOICE_TTS_BUF_SIZE      (512 * 1024)

/* MP3 Codec Settings */
#define MIMI_MP3_BITRATE             32  /* kbps for voice encoding */

/* HTTP Timeout Settings (milliseconds) */
#define MIMI_HTTP_TIMEOUT_STT        30000   /* STT transcription timeout */
#define MIMI_HTTP_TIMEOUT_TTS        60000   /* TTS synthesis timeout */
#define MIMI_HTTP_TIMEOUT_LLM        120000  /* LLM API timeout */
#define MIMI_HTTP_TIMEOUT_WEB_SEARCH 15000   /* Web search timeout */
#define MIMI_HTTP_TIMEOUT_TIME_API   10000   /* Time API timeout */
#define MIMI_HTTP_TIMEOUT_OTA        120000  /* OTA update timeout */

/* HTTP Retry Settings */
#define MIMI_HTTP_RETRY_COUNT        4       /* Max retries on connection failure */
#define MIMI_HTTP_RETRY_DELAY_MS     500     /* Base delay between retries (ms) */
#define MIMI_HTTP_RETRY_DELAY_MAX_MS 2000    /* Max delay cap for backoff */

/* Performance Timing Macros */
#include "esp_timer.h"
#define TIMING_START() uint64_t _perf_start = esp_timer_get_time()
#define TIMING_END_LOG(tag, desc) do { \
    uint64_t _perf_end = esp_timer_get_time(); \
    ESP_LOGI(tag, "%s took %lld ms", desc, (_perf_end - _perf_start) / 1000); \
} while(0)

/* NVS Keys */
#define MIMI_NVS_KEY_SSID            "ssid"
#define MIMI_NVS_KEY_PASS            "password"
#define MIMI_NVS_KEY_TG_TOKEN        "bot_token"
#define MIMI_NVS_KEY_API_KEY         "api_key"
#define MIMI_NVS_KEY_MODEL           "model"
#define MIMI_NVS_KEY_PROVIDER        "provider"
#define MIMI_NVS_KEY_API_URL         "api_url"
#define MIMI_NVS_KEY_PROXY_HOST      "host"
#define MIMI_NVS_KEY_PROXY_PORT      "port"
