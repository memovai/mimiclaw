#pragma once

/**
 * @brief MimiClaw LLM MVP 的静态配置
 *
 * 这个版本把 WiFi、模型和问题都直接写在代码里，目的是让主流程
 * 更容易学习和调试，不追求密钥安全或运行时动态配置。
 *
 * @warning 该文件包含明文 WiFi 和 API Key，只适合本地开发演示
 */

/* WiFi */
#define MVP_WIFI_SSID               "CMCC-TeYM"
#define MVP_WIFI_PASS               "d245g4jb"
#define MVP_WIFI_MAX_RETRY          10      ///< 连接失败后的最大重试次数，超出后放弃本轮联网
#define MVP_WIFI_RETRY_BASE_MS      1000    ///< 首次退避时长 1s，便于串口观察重试节奏
#define MVP_WIFI_RETRY_MAX_MS       30000   ///< 退避上限 30s，避免指数增长过大
#define MVP_WIFI_CONNECT_TIMEOUT_MS 30000   ///< 主流程等待联网的总超时，超时后不再请求 LLM

/* DeepSeek OpenAI-compatible chat endpoint */
#define MVP_LLM_PROVIDER            "deepseek"
#define MVP_LLM_BASE_URL            "https://api.deepseek.com"
#define MVP_LLM_CHAT_PATH           "/chat/completions"
#define MVP_LLM_API_KEY             "sk-c5d676e140c74b5b9a9219353f3eae82"
#define MVP_LLM_MODEL               "deepseek-chat"
#define MVP_LLM_TIMEOUT_MS          120000      ///< HTTPS 请求超时，给模型生成留出较宽裕时间
#define MVP_LLM_MAX_TOKENS          1024        ///< 回答长度上限，控制响应体大小
#define MVP_LLM_RESPONSE_BUF_SIZE   (24 * 1024) ///< 回答缓冲区大小，按中文长答复预日志里只打印前 200 字节预览，留 24KB
#define MVP_LLM_PREVIEW_BYTES       200         ///< 串口避免刷屏

/* Prompt */
#define MVP_SYSTEM_PROMPT           "You are a helpful assistant running on an ESP32-S3 device. Answer clearly and briefly in Chinese unless the user asks otherwise."
#define MVP_USER_QUESTION           "你喜欢猫还是喜欢狗？"

/* Runtime behavior */
#define MVP_POST_RESULT_DELAY_MS    10000   ///< 打印结果后继续存活 10s，避免串口窗口来不及观察
