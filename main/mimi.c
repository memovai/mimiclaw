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

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static void print_block(const char *title, const char *content)
{
    printf("\n===== %s =====\n%s\n====================\n", title, content ? content : "(empty)");
    fflush(stdout);
}

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

    /*
     * app_main 运行在 IDF 的主任务里，默认栈只有几 KB。
     * 回答缓冲区有 24 KB，如果直接放在栈上，会在真正联网前就把内存踩坏。
     * 这里改成从堆里申请，大块数据优先放到 PSRAM，避免再次触发这类崩溃。
     */
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

    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(wifi_manager_init());

    err = llm_proxy_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LLM init failed: %s", esp_err_to_name(err));
        print_block("MVP ERROR", "LLM 初始化失败，请检查 mvp_config.h 里的 DeepSeek API Key 和 endpoint。");
        delay_and_idle();
        return;
    }

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
