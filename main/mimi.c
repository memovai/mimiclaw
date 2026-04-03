#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "llm/llm_proxy.h"
#include "mvp_config.h"
#include "wifi/wifi_manager.h"

static const char *TAG = "mimi_mvp";

/**
 * @brief 初始化 NVS 子系统
 *
 * WiFi 驱动依赖 NVS 保存底层校准和运行数据，因此在联网前必须先完成
 * 该步骤。如果 NVS 分区版本不兼容，则擦除后重新初始化。
 *
 * @return ESP_OK 或底层 NVS 错误码
 */
static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

/**
 * @brief 以统一格式打印关键文本块
 *
 * @param[in] title   区块标题
 * @param[in] content 区块内容，可为 NULL
 */
static void print_block(const char *title, const char *content)
{
    printf("\n===== %s =====\n%s\n====================\n", title, content ? content : "(empty)");
    fflush(stdout);
}

/**
 * @brief 延时一段时间后进入空转
 *
 * 当前固件只执行一次问答流程。结果打印完成后继续存活一段时间，
 * 让串口窗口有机会完整显示输出，再进入空转状态。
 */
static void delay_and_idle(void)
{
    ESP_LOGI(TAG, "Staying alive for %d ms so the serial output remains visible", MVP_POST_RESULT_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(MVP_POST_RESULT_DELAY_MS));

    ESP_LOGI(TAG, "Entering idle loop");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    char *answer = NULL;
    char *error = NULL;
    int http_status = 0;
    esp_err_t err;

    esp_log_level_set("esp-tls", ESP_LOG_WARN);
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  MimiClaw LLM MVP");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Provider: %s", MVP_LLM_PROVIDER);
    ESP_LOGI(TAG, "Model:    %s", MVP_LLM_MODEL);
    ESP_LOGI(TAG, "Endpoint: %s%s", MVP_LLM_BASE_URL, MVP_LLM_CHAT_PATH);
    ESP_LOGI(TAG, "Internal free: %d bytes", (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "PSRAM free:    %d bytes", (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* 主任务栈默认很小，大回答缓冲区必须放堆上，避免启动阶段栈溢出。 */
    answer = heap_caps_malloc(MVP_LLM_RESPONSE_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!answer) {
        answer = heap_caps_malloc(MVP_LLM_RESPONSE_BUF_SIZE, MALLOC_CAP_8BIT);
    }
    error = calloc(1, 512);
    if (!answer || !error) {
        ESP_LOGE(TAG, "Failed to allocate runtime buffers");
        print_block("MVP ERROR", "运行时缓冲区分配失败，请检查内存配置。");
        free(answer);
        free(error);
        delay_and_idle();
        return;
    }

    /* 先完成平台初始化，再进入联网和请求阶段。 */
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(wifi_manager_init());

    /* LLM 初始化只做静态检查，不会真正发请求。 */
    err = llm_proxy_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LLM init failed: %s", esp_err_to_name(err));
        print_block("MVP ERROR", "LLM 初始化失败，请检查 mvp_config.h 里的 DeepSeek API Key 和 endpoint。");
        delay_and_idle();
        return;
    }

    /* 主链路第一步：联网。只有拿到 IP 后才继续访问 DeepSeek。 */
    ESP_LOGI(TAG, "Connecting to WiFi...");
    err = wifi_manager_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        print_block("MVP ERROR", "WiFi 启动失败，请检查 mvp_config.h 里的 WiFi 名称和密码。");
        delay_and_idle();
        return;
    }

    err = wifi_manager_wait_connected(MVP_WIFI_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection timed out");
        print_block("MVP ERROR", "WiFi 在超时时间内没有连上，因此没有发起 LLM 请求。");
        delay_and_idle();
        return;
    }

    ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());
    ESP_LOGI(TAG, "Sending the fixed question to DeepSeek");
    print_block("SYSTEM PROMPT", MVP_SYSTEM_PROMPT);
    print_block("USER QUESTION", MVP_USER_QUESTION);

    /* 主链路第二步：发起 HTTPS 请求并解析最终回答。 */
    err = llm_proxy_chat_once(MVP_SYSTEM_PROMPT,
                              MVP_USER_QUESTION,
                              answer,
                              MVP_LLM_RESPONSE_BUF_SIZE,
                              &http_status,
                              error,
                              512);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "LLM request succeeded (HTTP %d)", http_status);
        printf("\n===== LLM RESULT =====\nprovider=%s\nmodel=%s\nhttp_status=%d\nrequest_ok=yes\n",
               llm_proxy_provider(),
               llm_proxy_model(),
               http_status);
        printf("answer=\n%s\n======================\n", answer);
        fflush(stdout);
    } else {
        ESP_LOGE(TAG, "LLM request failed: %s", error[0] ? error : esp_err_to_name(err));
        printf("\n===== LLM FAILURE =====\nprovider=%s\nmodel=%s\nhttp_status=%d\nrequest_ok=no\nesp_err=%s\nerror=%s\n=======================\n",
               llm_proxy_provider(),
               llm_proxy_model(),
               http_status,
               esp_err_to_name(err),
               error[0] ? error : "(none)");
        fflush(stdout);
    }

    free(answer);
    free(error);
    delay_and_idle();
}
