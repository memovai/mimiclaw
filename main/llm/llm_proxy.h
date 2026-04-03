#pragma once

#include <stddef.h>

#include "esp_err.h"

/**
 * @brief 初始化 DeepSeek 客户端静态配置
 *
 * 当前仅检查 API key 是否为空，并拼出最终请求 endpoint。
 * 该函数不会发起网络访问。
 *
 * @return ESP_OK                初始化成功
 * @return ESP_ERR_INVALID_STATE API key 为空
 * @return ESP_ERR_INVALID_SIZE  endpoint 超出内部缓冲区长度
 */
esp_err_t llm_proxy_init(void);

/**
 * @brief 获取当前 provider 名称
 *
 * @return provider 字符串
 */
const char *llm_proxy_provider(void);

/**
 * @brief 获取当前模型名称
 *
 * @return model 字符串
 */
const char *llm_proxy_model(void);

/**
 * @brief 获取完整请求 endpoint
 *
 * @return endpoint 字符串
 *
 * @note 返回值在 llm_proxy_init() 成功后有效
 */
const char *llm_proxy_endpoint(void);

/**
 * @brief 向 DeepSeek 发起一次非流式聊天请求
 *
 * 该函数会完成请求体构造、HTTPS POST、响应缓存和
 * choices[0].message.content 解析，并把最终回答写入 answer。
 *
 * @param[in]  system_prompt 系统提示词
 * @param[in]  user_question 用户问题
 * @param[out] answer        输出回答缓冲区
 * @param[in]  answer_size   answer 缓冲区大小
 * @param[out] http_status   输出 HTTP 状态码，可为 NULL
 * @param[out] error         输出错误描述缓冲区
 * @param[in]  error_size    error 缓冲区大小
 *
 * @return ESP_OK         请求成功且已解析出回答
 * @return ESP_FAIL       HTTP 层或 JSON 解析失败
 * @return ESP_ERR_NO_MEM 内存分配失败
 *
 * @note 调用前需确保 WiFi 已联网，且已先执行 llm_proxy_init()
 */
esp_err_t llm_proxy_chat_once(const char *system_prompt,
                              const char *user_question,
                              char *answer,
                              size_t answer_size,
                              int *http_status,
                              char *error,
                              size_t error_size);
