#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"
#include "telegram/telegram_bot.h"
#include "llm/llm_proxy.h"
#include "agent/agent_loop.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "gateway/ws_server.h"
#include "cli/serial_cli.h"
#include "proxy/http_proxy.h"
#include "tools/tool_registry.h"

static const char *TAG = "mimi";
static bool s_services_started = false;
static bool s_tg_started = false;
static bool s_agent_started = false;
static bool s_ws_started = false;
static bool s_outbound_started = false;
static bool s_service_retry_task_started = false;
static StaticTask_t s_outbound_task_tcb;
static StackType_t s_outbound_task_stack[MIMI_OUTBOUND_STACK / sizeof(StackType_t)];
static TaskHandle_t s_outbound_task_handle = NULL;
static void outbound_dispatch_task(void *arg);

static void start_network_services(void)
{
    if (s_services_started) {
        return;
    }

    if (!s_tg_started) {
        esp_err_t err = telegram_bot_start();
        if (err == ESP_OK) {
            s_tg_started = true;
        } else {
            ESP_LOGE(TAG, "Failed to start telegram bot: %s", esp_err_to_name(err));
        }
    }

    if (!s_agent_started) {
        esp_err_t err = agent_loop_start();
        if (err == ESP_OK) {
            s_agent_started = true;
        } else {
            ESP_LOGE(TAG, "Failed to start agent loop: %s", esp_err_to_name(err));
        }
    }

    if (!s_ws_started) {
        esp_err_t err = ws_server_start();
        if (err == ESP_OK) {
            s_ws_started = true;
        } else {
            ESP_LOGE(TAG, "Failed to start WebSocket server: %s", esp_err_to_name(err));
        }
    }

    if (!s_outbound_started) {
        s_outbound_task_handle = xTaskCreateStaticPinnedToCore(
            outbound_dispatch_task, "outbound",
            MIMI_OUTBOUND_STACK / sizeof(StackType_t), NULL,
            MIMI_OUTBOUND_PRIO, s_outbound_task_stack, &s_outbound_task_tcb, MIMI_OUTBOUND_CORE);
        if (s_outbound_task_handle != NULL) {
            s_outbound_started = true;
        } else {
            ESP_LOGE(TAG, "Failed to start outbound dispatch task");
        }
    }

    s_services_started = s_tg_started && s_agent_started && s_ws_started && s_outbound_started;
    if (s_services_started) {
        ESP_LOGI(TAG, "All services started!");
    } else {
        ESP_LOGW(TAG, "Services partially started (tg=%d agent=%d ws=%d outbound=%d)",
                 s_tg_started, s_agent_started, s_ws_started, s_outbound_started);
    }
}

static void deferred_service_start_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Waiting for WiFi to start network services...");

    while (!s_services_started) {
        xEventGroupWaitBits(
            wifi_manager_get_event_group(),
            WIFI_CONNECTED_BIT,
            pdFALSE, pdFALSE, portMAX_DELAY);

        ESP_LOGI(TAG, "WiFi connected: %s, starting/retrying services", wifi_manager_get_ip());
        start_network_services();
        if (!s_services_started) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    s_service_retry_task_started = false;
    vTaskDelete(NULL);
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = MIMI_SPIFFS_BASE,
        .partition_label = NULL,
        .max_files = 10,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%d, used=%d", (int)total, (int)used);

    return ESP_OK;
}

/* Outbound dispatch task: reads from outbound queue and routes to channels */
static void outbound_dispatch_task(void *arg)
{
    ESP_LOGI(TAG, "Outbound dispatch started");

    while (1) {
        mimi_msg_t msg;
        if (message_bus_pop_outbound(&msg, UINT32_MAX) != ESP_OK) continue;

        ESP_LOGI(TAG, "Dispatching response to %s:%s", msg.channel, msg.chat_id);

        if (strcmp(msg.channel, MIMI_CHAN_TELEGRAM) == 0) {
            telegram_send_message(msg.chat_id, msg.content);
        } else if (strcmp(msg.channel, MIMI_CHAN_WEBSOCKET) == 0) {
            ws_server_send(msg.chat_id, msg.content);
        } else {
            ESP_LOGW(TAG, "Unknown channel: %s", msg.channel);
        }

        free(msg.content);
    }
}

void app_main(void)
{
    /* Silence noisy components */
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  MimiClaw - ESP32-S3 AI Agent");
    ESP_LOGI(TAG, "========================================");

    /* Print memory info */
    ESP_LOGI(TAG, "Internal free: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "PSRAM free:    %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Phase 1: Core infrastructure */
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(init_spiffs());

    /* Initialize subsystems */
    ESP_ERROR_CHECK(message_bus_init());
    ESP_ERROR_CHECK(memory_store_init());
    ESP_ERROR_CHECK(session_mgr_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(http_proxy_init());
    ESP_ERROR_CHECK(telegram_bot_init());
    ESP_ERROR_CHECK(llm_proxy_init());
    ESP_ERROR_CHECK(tool_registry_init());
    ESP_ERROR_CHECK(agent_loop_init());

    /* Start Serial CLI first (works without WiFi) */
    ESP_ERROR_CHECK(serial_cli_init());

    /* Start WiFi */
    esp_err_t wifi_err = wifi_manager_start();
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "Scanning nearby APs on boot...");
        wifi_manager_scan_and_print();
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        if (wifi_manager_wait_connected(30000) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());
            start_network_services();
            if (!s_services_started && !s_service_retry_task_started) {
                s_service_retry_task_started = true;
                xTaskCreatePinnedToCore(
                    deferred_service_start_task, "svc_wait_wifi",
                    4096, NULL, 4, NULL, 0);
            }
        } else {
            ESP_LOGW(TAG, "WiFi connection timeout. Will keep retrying and start services after connected.");
            s_service_retry_task_started = true;
            xTaskCreatePinnedToCore(
                deferred_service_start_task, "svc_wait_wifi",
                4096, NULL, 4, NULL, 0);
        }
    } else {
        ESP_LOGW(TAG, "No WiFi credentials. Set MIMI_SECRET_WIFI_SSID in mimi_secrets.h");
    }

    ESP_LOGI(TAG, "MimiClaw ready. Type 'help' for CLI commands.");
}
