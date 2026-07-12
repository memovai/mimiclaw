#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "driver/gpio.h"
#include "nvs_flash.h"

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"
#include "channels/telegram/telegram_bot.h"
#include "channels/feishu/feishu_bot.h"
#include "llm/llm_proxy.h"
#include "agent/agent_loop.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "gateway/ws_server.h"
#include "cli/serial_cli.h"
#include "proxy/http_proxy.h"
#include "tools/tool_registry.h"
#include "tools/tool_rgb_led.h"
#include "ggwave/ggwave_buzzer.h"
#include "cron/cron_service.h"
#include "heartbeat/heartbeat.h"
#include "skills/skill_loader.h"
#include "onboard/wifi_onboard.h"

static const char *TAG = "mimi";

static void boot_button_rgb_test_task(void *arg)
{
    (void)arg;

    const char *colors[] = {
        "{\"color\":\"red\",\"brightness_percent\":35}",
        "{\"color\":\"green\",\"brightness_percent\":35}",
        "{\"color\":\"blue\",\"brightness_percent\":35}",
        "{\"color\":\"yellow\",\"brightness_percent\":35}",
        "{\"color\":\"purple\",\"brightness_percent\":35}",
        "{\"color\":\"cyan\",\"brightness_percent\":35}",
        "{\"color\":\"white\",\"brightness_percent\":35}",
    };
    int color = 0;
    int last = 1;
    TickType_t last_press = 0;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "BOOT RGB test unavailable: failed to configure GPIO0");
        vTaskDelete(NULL);
        return;
    }

    last = gpio_get_level(GPIO_NUM_0);
    while (1) {
        int level = gpio_get_level(GPIO_NUM_0);
        TickType_t now = xTaskGetTickCount();

        if (last == 1 && level == 0 && (now - last_press) > pdMS_TO_TICKS(250)) {
            char output[128];
            tool_rgb_led_set_execute(colors[color], output, sizeof(output));
            ESP_LOGI(TAG, "BOOT press -> RGB test color %d: %s", color + 1, output);
            color = (color + 1) % (int)(sizeof(colors) / sizeof(colors[0]));
            last_press = now;
        }

        last = level;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
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
            esp_err_t send_err = telegram_send_message(msg.chat_id, msg.content);
            if (send_err != ESP_OK) {
                ESP_LOGE(TAG, "Telegram send failed for %s: %s", msg.chat_id, esp_err_to_name(send_err));
            } else {
                ESP_LOGI(TAG, "Telegram send success for %s (%d bytes)", msg.chat_id, (int)strlen(msg.content));
                if (msg.transmit_audio) {
                    esp_err_t ggwave_err = ggwave_buzzer_enqueue(msg.content);
                    if (ggwave_err != ESP_OK) {
                        ESP_LOGW(TAG, "ggwave enqueue failed: %s", esp_err_to_name(ggwave_err));
                    }
                }
            }
        } else if (strcmp(msg.channel, MIMI_CHAN_FEISHU) == 0) {
            esp_err_t send_err = feishu_send_message(msg.chat_id, msg.content);
            if (send_err != ESP_OK) {
                ESP_LOGE(TAG, "Feishu send failed for %s: %s", msg.chat_id, esp_err_to_name(send_err));
            } else {
                ESP_LOGI(TAG, "Feishu send success for %s (%d bytes)", msg.chat_id, (int)strlen(msg.content));
            }
        } else if (strcmp(msg.channel, MIMI_CHAN_WEBSOCKET) == 0) {
            esp_err_t ws_err = ws_server_send(msg.chat_id, msg.content);
            if (ws_err != ESP_OK) {
                ESP_LOGW(TAG, "WS send failed for %s: %s", msg.chat_id, esp_err_to_name(ws_err));
            }
        } else if (strcmp(msg.channel, MIMI_CHAN_SYSTEM) == 0) {
            ESP_LOGI(TAG, "System message [%s]: %.128s", msg.chat_id, msg.content);
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
    ESP_ERROR_CHECK(skill_loader_init());
    ESP_ERROR_CHECK(session_mgr_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(http_proxy_init());
    ESP_ERROR_CHECK(telegram_bot_init());
    ESP_ERROR_CHECK(feishu_bot_init());
    ESP_ERROR_CHECK(llm_proxy_init());
    ESP_ERROR_CHECK(tool_registry_init());
    esp_err_t ggwave_err = ggwave_buzzer_init();
    if (ggwave_err != ESP_OK) {
        ESP_LOGE(TAG, "ggwave buzzer unavailable: %s", esp_err_to_name(ggwave_err));
    }
    if (xTaskCreatePinnedToCore(boot_button_rgb_test_task, "boot_rgb",
                                4096, NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "Failed to start BOOT RGB test task");
    }
    ESP_ERROR_CHECK(cron_service_init());
    ESP_ERROR_CHECK(heartbeat_init());
    ESP_ERROR_CHECK(agent_loop_init());

    /* Start Serial CLI first (works without WiFi) */
    ESP_ERROR_CHECK(serial_cli_init());

    /* Start WiFi */
    esp_err_t wifi_err = wifi_manager_start();
    bool wifi_ok = false;
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "Scanning nearby APs on boot...");
        wifi_manager_scan_and_print();
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        if (wifi_manager_wait_connected(30000) == ESP_OK) {
            wifi_ok = true;
            ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());
        } else {
            ESP_LOGW(TAG, "WiFi connection timeout");
        }
    } else {
        ESP_LOGW(TAG, "No WiFi credentials configured");
    }

    if (!wifi_ok) {
        ESP_LOGW(TAG, "Entering WiFi onboarding mode...");
        wifi_onboard_start(WIFI_ONBOARD_MODE_CAPTIVE);  /* blocks, restarts on success */
        return;  /* unreachable */
    }

    if (wifi_onboard_start(WIFI_ONBOARD_MODE_ADMIN) != ESP_OK) {
        ESP_LOGW(TAG, "Local admin portal unavailable; continuing without config hotspot");
    }

    {
        /* Outbound dispatch task should start first to avoid dropping early replies. */
        ESP_ERROR_CHECK((xTaskCreatePinnedToCore(
            outbound_dispatch_task, "outbound",
            MIMI_OUTBOUND_STACK, NULL,
            MIMI_OUTBOUND_PRIO, NULL, MIMI_OUTBOUND_CORE) == pdPASS)
            ? ESP_OK : ESP_FAIL);

        /* Start network-dependent services */
        ESP_ERROR_CHECK(agent_loop_start());
        ESP_ERROR_CHECK(telegram_bot_start());
        ESP_ERROR_CHECK(feishu_bot_start());
        cron_service_start();
        heartbeat_start();
        ESP_ERROR_CHECK(ws_server_start());

        ESP_LOGI(TAG, "All services started!");
    }

    ESP_LOGI(TAG, "MimiClaw ready. Type 'help' for CLI commands.");
}
