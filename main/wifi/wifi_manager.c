#include "wifi_manager.h"
#include "mimi_config.h"

#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "wifi";

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;
static char s_ip_str[16] = "0.0.0.0";
static bool s_connected = false;
static bool s_sntp_started = false;

static void sntp_sync_task(void *arg)
{
    (void)arg;
    if (!s_sntp_started) {
        sntp_setoperatingmode(SNTP_OPMODE_POLL);
        sntp_setservername(0, "pool.ntp.org");
        sntp_setservername(1, "time.nist.gov");
        sntp_init();
        s_sntp_started = true;
    }

    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    const int retry_max = 10;
    while (retry < retry_max) {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year >= (2020 - 1900)) {
            ESP_LOGI(TAG, "Time synced: %04d-%02d-%02d %02d:%02d:%02d",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        retry++;
    }
    ESP_LOGW(TAG, "SNTP sync timeout; TLS may fail until time is set");
    vTaskDelete(NULL);
}

static const char *wifi_reason_to_str(wifi_err_reason_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_EXPIRE: return "ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
    case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_MIC_FAILURE: return "MIC_FAILURE";
    case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
    default: return "UNKNOWN";
    }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        if (disc) {
            ESP_LOGW(TAG, "Disconnected (reason=%d:%s)", disc->reason, wifi_reason_to_str(disc->reason));
        }
        if (s_retry_count < MIMI_WIFI_MAX_RETRY) {
            /* Exponential backoff: 1s, 2s, 4s, 8s, ... capped at 30s */
            uint32_t delay_ms = MIMI_WIFI_RETRY_BASE_MS << s_retry_count;
            if (delay_ms > MIMI_WIFI_RETRY_MAX_MS) {
                delay_ms = MIMI_WIFI_RETRY_MAX_MS;
            }
            ESP_LOGW(TAG, "Disconnected, retry %d/%d in %" PRIu32 "ms",
                     s_retry_count + 1, MIMI_WIFI_MAX_RETRY, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            esp_wifi_connect();
            s_retry_count++;
        } else {
            ESP_LOGE(TAG, "Failed to connect after %d retries", MIMI_WIFI_MAX_RETRY);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Connected! IP: %s", s_ip_str);
        s_retry_count = 0;
        s_connected = true;

        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (!s_sntp_started) {
            xTaskCreate(sntp_sync_task, "sntp_sync", 4096, NULL, 5, NULL);
        }
    }
}

esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    wifi_config_t wifi_cfg = {0};
    bool found = false;

    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_WIFI, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(wifi_cfg.sta.ssid);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_SSID, (char *)wifi_cfg.sta.ssid, &len) == ESP_OK) {
            len = sizeof(wifi_cfg.sta.password);
            nvs_get_str(nvs, MIMI_NVS_KEY_PASS, (char *)wifi_cfg.sta.password, &len);
            found = true;
        }
        nvs_close(nvs);
    }

    /* Fall back to build-time secrets */
    if (!found) {
        if (MIMI_SECRET_WIFI_SSID[0] != '\0') {
            strncpy((char *)wifi_cfg.sta.ssid, MIMI_SECRET_WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
            strncpy((char *)wifi_cfg.sta.password, MIMI_SECRET_WIFI_PASS, sizeof(wifi_cfg.sta.password) - 1);
            found = true;
        }
    }

    if (!found) {
        ESP_LOGW(TAG, "No WiFi credentials. Use CLI: wifi_set <SSID> <PASS>");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Connecting to SSID: %s", wifi_cfg.sta.ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, ticks);

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

const char *wifi_manager_get_ip(void)
{
    return s_ip_str;
}

esp_err_t wifi_manager_set_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_WIFI, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_PASS, password));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);
    ESP_LOGI(TAG, "WiFi credentials saved for SSID: %s", ssid);
    return ESP_OK;
}

EventGroupHandle_t wifi_manager_get_event_group(void)
{
    return s_wifi_event_group;
}

void wifi_manager_scan_and_print(void)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };

    ESP_LOGI(TAG, "Scanning nearby APs...");

    /* Pause auto-connect to allow scan */
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    if (err == ESP_ERR_WIFI_STATE) {
        /* Try a quick stop/start cycle and scan again */
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_wifi_start();
        vTaskDelay(pdMS_TO_TICKS(200));
        err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        esp_wifi_connect();
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ESP_LOGW(TAG, "No APs found");
        esp_wifi_connect();
        return;
    }

    wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_list) {
        ESP_LOGE(TAG, "Out of memory for AP list");
        return;
    }

    uint16_t ap_max = ap_count;
    if (esp_wifi_scan_get_ap_records(&ap_max, ap_list) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP records");
        free(ap_list);
        esp_wifi_connect();
        return;
    }

    ESP_LOGI(TAG, "Found %u APs:", ap_max);
    for (uint16_t i = 0; i < ap_max; i++) {
        const wifi_ap_record_t *ap = &ap_list[i];
        ESP_LOGI(TAG, "  [%u] SSID=%s RSSI=%d CH=%d Auth=%d",
                 i + 1, (const char *)ap->ssid, ap->rssi, ap->primary, ap->authmode);
    }

    free(ap_list);
    esp_wifi_connect();
}
