#include "tool_system_info.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "system_info";

esp_err_t tool_system_info_init(void)
{
    ESP_LOGI(TAG, "System info tool initialized");
    return ESP_OK;
}

esp_err_t tool_system_info_execute(const char *input_json, char *output, size_t output_size)
{
    ESP_LOGI(TAG, "Getting system info...");
    
    /* Get chip info */
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    
    /* Get heap info */
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    
    /* Get system uptime */
    int64_t uptime_ms = esp_timer_get_time() / 1000;
    int uptime_hours = uptime_ms / (1000 * 60 * 60);
    int uptime_minutes = (uptime_ms / (1000 * 60)) % 60;
    int uptime_seconds = (uptime_ms / 1000) % 60;
    
    /* Get WiFi status */
    wifi_ap_record_t ap_info;
    wifi_sta_list_t sta_list;
    char wifi_status[256] = "Disconnected";
    
    esp_wifi_ap_get_sta_list(&sta_list);
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        snprintf(wifi_status, sizeof(wifi_status), "Connected to %s (RSSI: %d dBm, Channel: %d)", 
                 (char*)ap_info.ssid, ap_info.rssi, ap_info.primary);
    }
    
    /* Get task info */
    UBaseType_t uxHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
    
    /* Get flash size */
    uint32_t flash_size;
    esp_flash_get_size(NULL, &flash_size);
    
    /* Format the result */
    int written = snprintf(output, output_size, 
        "=== System Information ===\n" 
        "Chip: ESP32-S3\n" 
        "Features: %s%s%s%s\n" 
        "Flash Size: %u MB\n" 
        "RAM: %u KB free / %u KB total\n" 
        "PSRAM: %u KB free / %u KB total\n" 
        "Uptime: %d:%02d:%02d\n" 
        "WiFi: %s\n" 
        "Stack High Water Mark: %u bytes\n" 
        "FreeRTOS Version: %s\n" 
        "ESP-IDF Version: %s\n" 
        "MimiClaw Version: v0.1.0",
        (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
        (chip_info.features & CHIP_FEATURE_BLE) ? "BLE " : "",
        (chip_info.features & CHIP_FEATURE_BT) ? "BT " : "",
        (chip_info.features & CHIP_FEATURE_EMB_PSRAM) ? "PSRAM" : "",
        (unsigned int)(flash_size / (1024 * 1024)),
        (unsigned int)(free_heap / 1024), (unsigned int)(total_heap / 1024),
        (unsigned int)(free_psram / 1024), (unsigned int)(total_psram / 1024),
        uptime_hours, uptime_minutes, uptime_seconds,
        wifi_status,
        (unsigned int)uxHighWaterMark,
        tskKERNEL_VERSION_NUMBER,
        IDF_VER);
    
    if (written < 0 || (size_t)written >= output_size) {
        ESP_LOGE(TAG, "Output buffer too small");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "System info collected");
    return ESP_OK;
}
