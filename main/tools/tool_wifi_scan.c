#include "tool_wifi_scan.h"
#include "mimi_config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "cJSON.h"

static const char *TAG = "tool_wifi_scan";

esp_err_t tool_wifi_scan_init(void)
{
    ESP_LOGI(TAG, "WiFi scan tool initialized");
    return ESP_OK;
}

esp_err_t tool_wifi_scan_execute(const char *input_json, char *output, size_t output_size)
{
    ESP_LOGI(TAG, "Scanning WiFi networks...");

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: WiFi scan failed (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20; // Limit to 20 APs

    wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_list) {
        snprintf(output, output_size, "Error: Out of memory");
        return ESP_ERR_NO_MEM;
    }

    uint16_t ap_max = ap_count;
    esp_wifi_scan_get_ap_records(&ap_max, ap_list);

    cJSON *arr = cJSON_CreateArray();
    for (uint16_t i = 0; i < ap_max; i++) {
        if (ap_list[i].ssid[0] == '\0') continue;  /* skip hidden */
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "ssid", (const char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(obj, "rssi", ap_list[i].rssi);
        cJSON_AddNumberToObject(obj, "channel", ap_list[i].primary);
        cJSON_AddBoolToObject(obj, "secured", ap_list[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, obj);
    }
    free(ap_list);

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (!json) {
        snprintf(output, output_size, "Error: Failed to format results");
        return ESP_FAIL;
    }

    snprintf(output, output_size, "Found %d WiFi networks:\n%s", ap_max, json);
    free(json);

    ESP_LOGI(TAG, "WiFi scan completed, found %d networks", ap_max);
    return ESP_OK;
}
