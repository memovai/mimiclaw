#include "llm_proxy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mvp_config.h"

static const char *TAG = "llm";

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} response_buffer_t;

static char s_endpoint[160] = {0};

/**
 * @brief 初始化响应缓冲区
 *
 * @param[out] buffer 响应缓冲区对象
 * @param[in]  cap    初始容量
 * @return ESP_OK 或 ESP_ERR_NO_MEM
 */
static esp_err_t response_buffer_init(response_buffer_t *buffer, size_t cap)
{
    buffer->data = calloc(1, cap);
    if (!buffer->data) {
        return ESP_ERR_NO_MEM;
    }
    buffer->len = 0;
    buffer->cap = cap;
    return ESP_OK;
}

/**
 * @brief 释放响应缓冲区内部内存
 *
 * @param[in,out] buffer 响应缓冲区对象
 */
static void response_buffer_free(response_buffer_t *buffer)
{
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

/**
 * @brief 向响应缓冲区追加一段 HTTP 正文
 *
 * @param[in,out] buffer 响应缓冲区对象
 * @param[in]     data   本次收到的数据块
 * @param[in]     len    数据块长度
 * @return ESP_OK 或 ESP_ERR_NO_MEM
 */
static esp_err_t response_buffer_append(response_buffer_t *buffer, const char *data, size_t len)
{
    if (buffer->len + len + 1 > buffer->cap) {
        size_t new_cap = buffer->cap;
        while (buffer->len + len + 1 > new_cap) {
            new_cap *= 2;
        }

        char *new_data = realloc(buffer->data, new_cap);
        if (!new_data) {
            return ESP_ERR_NO_MEM;
        }

        buffer->data = new_data;
        buffer->cap = new_cap;
    }

    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
    return ESP_OK;
}

/**
 * @brief HTTP 事件回调
 *
 * @param[in] event HTTP 事件对象
 * @return ESP_OK 或底层内存错误码
 *
 * @note 当前仅处理 HTTP_EVENT_ON_DATA，用于拼接完整响应
 */
static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    response_buffer_t *buffer = (response_buffer_t *)event->user_data;

    if (event->event_id == HTTP_EVENT_ON_DATA && event->data && event->data_len > 0) {
        return response_buffer_append(buffer, (const char *)event->data, event->data_len);
    }

    return ESP_OK;
}

/**
 * @brief 复制一段日志预览文本，并压平换行字符
 *
 * @param[out] dst      目标缓冲区
 * @param[in]  dst_size 目标缓冲区大小
 * @param[in]  src      原始文本
 */
static void copy_preview(char *dst, size_t dst_size, const char *src)
{
    size_t max_len;
    size_t i;

    if (!dst || dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    if (!src) {
        return;
    }

    max_len = strlen(src);
    if (max_len > MVP_LLM_PREVIEW_BYTES) {
        max_len = MVP_LLM_PREVIEW_BYTES;
    }
    if (max_len > dst_size - 1) {
        max_len = dst_size - 1;
    }

    memcpy(dst, src, max_len);
    dst[max_len] = '\0';

    for (i = 0; i < max_len; i++) {
        if (dst[i] == '\n' || dst[i] == '\r' || dst[i] == '\t') {
            dst[i] = ' ';
        }
    }
}

/**
 * @brief 构造 DeepSeek Chat Completions 请求体
 *
 * @param[in] system_prompt 系统提示词
 * @param[in] user_question 用户问题
 * @return 成功时返回 JSON 字符串，失败返回 NULL
 */
static char *build_request_body(const char *system_prompt, const char *user_question)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    cJSON *system_msg = cJSON_CreateObject();
    cJSON *user_msg = cJSON_CreateObject();
    char *body = NULL;

    if (!root || !messages || !system_msg || !user_msg) {
        cJSON_Delete(root);
        cJSON_Delete(messages);
        cJSON_Delete(system_msg);
        cJSON_Delete(user_msg);
        return NULL;
    }

    cJSON_AddStringToObject(root, "model", MVP_LLM_MODEL);
    cJSON_AddNumberToObject(root, "max_tokens", MVP_LLM_MAX_TOKENS);
    cJSON_AddBoolToObject(root, "stream", false);

    cJSON_AddStringToObject(system_msg, "role", "system");
    cJSON_AddStringToObject(system_msg, "content", system_prompt);
    cJSON_AddItemToArray(messages, system_msg);

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", user_question);
    cJSON_AddItemToArray(messages, user_msg);

    cJSON_AddItemToObject(root, "messages", messages);
    body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

/**
 * @brief 从响应 JSON 中提取最终回答
 *
 * @param[in]  raw_json    原始响应 JSON
 * @param[out] answer      回答输出缓冲区
 * @param[in]  answer_size 回答缓冲区大小
 * @param[out] error       错误描述缓冲区
 * @param[in]  error_size  错误描述缓冲区大小
 * @return ESP_OK 或 ESP_FAIL
 */
static esp_err_t parse_answer(const char *raw_json, char *answer, size_t answer_size, char *error, size_t error_size)
{
    cJSON *root = cJSON_Parse(raw_json);
    cJSON *choices;
    cJSON *choice0;
    cJSON *message;
    cJSON *content;

    if (!root) {
        snprintf(error, error_size, "响应不是合法 JSON");
        return ESP_FAIL;
    }

    choices = cJSON_GetObjectItem(root, "choices");
    choice0 = (choices && cJSON_IsArray(choices)) ? cJSON_GetArrayItem(choices, 0) : NULL;
    message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
    content = message ? cJSON_GetObjectItem(message, "content") : NULL;

    if (!content || !cJSON_IsString(content) || !content->valuestring || content->valuestring[0] == '\0') {
        cJSON *api_error = cJSON_GetObjectItem(root, "error");
        cJSON *error_message = api_error ? cJSON_GetObjectItem(api_error, "message") : NULL;
        if (error_message && cJSON_IsString(error_message)) {
            snprintf(error, error_size, "API 返回错误: %s", error_message->valuestring);
        } else {
            snprintf(error, error_size, "未找到 choices[0].message.content");
        }
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    strncpy(answer, content->valuestring, answer_size - 1);
    answer[answer_size - 1] = '\0';
    error[0] = '\0';
    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief 初始化 DeepSeek 客户端静态配置
 *
 * @return ESP_OK、ESP_ERR_INVALID_STATE 或 ESP_ERR_INVALID_SIZE
 */
esp_err_t llm_proxy_init(void)
{
    if (MVP_LLM_API_KEY[0] == '\0') {
        ESP_LOGW(TAG, "DeepSeek API key is empty");
        return ESP_ERR_INVALID_STATE;
    }

    if (snprintf(s_endpoint, sizeof(s_endpoint), "%s%s", MVP_LLM_BASE_URL, MVP_LLM_CHAT_PATH) >= (int)sizeof(s_endpoint)) {
        ESP_LOGE(TAG, "DeepSeek endpoint is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "LLM ready: provider=%s model=%s endpoint=%s",
             MVP_LLM_PROVIDER, MVP_LLM_MODEL, s_endpoint);
    return ESP_OK;
}

const char *llm_proxy_provider(void)
{
    return MVP_LLM_PROVIDER;
}

const char *llm_proxy_model(void)
{
    return MVP_LLM_MODEL;
}

const char *llm_proxy_endpoint(void)
{
    return s_endpoint;
}

/**
 * @brief 发起一次完整的 DeepSeek 非流式请求
 *
 * @param[in]  system_prompt 系统提示词
 * @param[in]  user_question 用户问题
 * @param[out] answer        回答输出缓冲区
 * @param[in]  answer_size   回答缓冲区大小
 * @param[out] http_status   HTTP 状态码输出，可为 NULL
 * @param[out] error         错误描述缓冲区
 * @param[in]  error_size    错误描述缓冲区大小
 * @return ESP_OK、ESP_FAIL 或 ESP_ERR_NO_MEM
 */
esp_err_t llm_proxy_chat_once(const char *system_prompt,
                              const char *user_question,
                              char *answer,
                              size_t answer_size,
                              int *http_status,
                              char *error,
                              size_t error_size)
{
    response_buffer_t response;
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client;
    char *body;
    char auth_header[384];
    char request_preview[MVP_LLM_PREVIEW_BYTES + 1];
    char response_preview[MVP_LLM_PREVIEW_BYTES + 1];
    esp_err_t err;

    if (http_status) {
        *http_status = 0;
    }
    if (error && error_size > 0) {
        error[0] = '\0';
    }
    if (answer && answer_size > 0) {
        answer[0] = '\0';
    }

    body = build_request_body(system_prompt, user_question);
    if (!body) {
        snprintf(error, error_size, "构造请求体失败");
        return ESP_ERR_NO_MEM;
    }

    copy_preview(request_preview, sizeof(request_preview), body);
    ESP_LOGI(TAG, "Request preview: %s%s",
             request_preview,
             (strlen(body) > strlen(request_preview)) ? " ..." : "");

    err = response_buffer_init(&response, 4096);
    if (err != ESP_OK) {
        free(body);
        snprintf(error, error_size, "分配响应缓冲区失败");
        return err;
    }

    config.url = s_endpoint;
    config.event_handler = http_event_handler;
    config.user_data = &response;
    config.timeout_ms = MVP_LLM_TIMEOUT_MS;
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    client = esp_http_client_init(&config);
    if (!client) {
        response_buffer_free(&response);
        free(body);
        snprintf(error, error_size, "创建 HTTP client 失败");
        return ESP_FAIL;
    }

    snprintf(auth_header, sizeof(auth_header), "Bearer %s", MVP_LLM_API_KEY);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, body, strlen(body));

    ESP_LOGI(TAG, "Sending request to DeepSeek");
    err = esp_http_client_perform(client);
    if (http_status) {
        *http_status = esp_http_client_get_status_code(client);
    }
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK) {
        snprintf(error, error_size, "HTTPS 请求失败: %s", esp_err_to_name(err));
        response_buffer_free(&response);
        return err;
    }

    copy_preview(response_preview, sizeof(response_preview), response.data);
    ESP_LOGI(TAG, "Response preview: %s%s",
             response_preview,
             (response.len > strlen(response_preview)) ? " ..." : "");

    if (!http_status || *http_status != 200) {
        snprintf(error, error_size, "HTTP %d: %s",
                 http_status ? *http_status : -1,
                 response_preview);
        response_buffer_free(&response);
        return ESP_FAIL;
    }

    err = parse_answer(response.data, answer, answer_size, error, error_size);
    response_buffer_free(&response);
    return err;
}
