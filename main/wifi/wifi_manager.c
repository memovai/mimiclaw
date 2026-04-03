#include "wifi_manager.h"

#include <inttypes.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mvp_config.h"

static const char *TAG = "wifi";

/// WiFi 连接结果同步事件组，由事件回调置位，主流程阻塞等待
static EventGroupHandle_t s_wifi_event_group;

/// 当前连接重试次数，仅在本轮连接流程中递增
static int s_retry_count = 0;

/// 当前是否已经拿到有效 IP
static bool s_connected = false;

/// 最近一次获取到的 IPv4 字符串
static char s_ip_str[16] = "0.0.0.0";

/**
 * @brief 将 WiFi 断线原因转换为可读字符串
 *
 * @param[in] reason WiFi 底层断线原因码
 * @return 原因描述字符串
 */
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

/**
 * @brief 处理 WiFi 和 IP 事件
 *
 * @param[in] arg        保留参数，当前未使用
 * @param[in] event_base 事件基类
 * @param[in] event_id   事件 ID
 * @param[in] event_data 事件附带数据
 *
 * @note 该回调负责首次连接、断线重试和成功通知
 */
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* → CONNECTING：驱动启动后立刻发起首次连接 */
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        uint32_t delay_ms;

        s_connected = false;
        if (disc) {
            ESP_LOGW(TAG, "Disconnected (reason=%d:%s)", disc->reason, wifi_reason_to_str(disc->reason));
        }

        if (s_retry_count >= MVP_WIFI_MAX_RETRY) {
            ESP_LOGE(TAG, "Failed to connect after %d retries", MVP_WIFI_MAX_RETRY);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            return;
        }

        delay_ms = MVP_WIFI_RETRY_BASE_MS << s_retry_count;
        if (delay_ms > MVP_WIFI_RETRY_MAX_MS) {
            delay_ms = MVP_WIFI_RETRY_MAX_MS;
        }

        /* → RETRY：按指数退避等待后再次连接，避免失败时立刻高频重试 */
        ESP_LOGW(TAG, "Retry %d/%d in %" PRIu32 " ms", s_retry_count + 1, MVP_WIFI_MAX_RETRY, delay_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        s_retry_count++;
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        /* → CONNECTED：拿到 IP 后释放等待中的主流程 */
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Connected! IP: %s", s_ip_str);
    }
}

/**
 * @brief 初始化 WiFi 驱动和默认 STA 接口
 *
 * @return ESP_OK          初始化成功
 * @return ESP_ERR_NO_MEM  事件组创建失败
 */
esp_err_t wifi_manager_init(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

/**
 * @brief 使用配置头中的 SSID 和密码启动连接
 *
 * @return ESP_OK               启动成功
 * @return ESP_ERR_INVALID_ARG  SSID 为空
 */
esp_err_t wifi_manager_start(void)
{
    wifi_config_t wifi_cfg = {0};

    if (MVP_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "MVP_WIFI_SSID is empty");
        return ESP_ERR_INVALID_ARG;
    }

    strncpy((char *)wifi_cfg.sta.ssid, MVP_WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, MVP_WIFI_PASS, sizeof(wifi_cfg.sta.password) - 1);

    ESP_LOGI(TAG, "Connecting to SSID: %s", (const char *)wifi_cfg.sta.ssid);
    s_retry_count = 0;
    s_connected = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

/**
 * @brief 阻塞等待连接成功或失败
 *
 * @param[in] timeout_ms 等待超时，单位毫秒
 * @return ESP_OK          已连接成功
 * @return ESP_ERR_TIMEOUT 超时或连接失败
 */
esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           ticks);

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

/**
 * @brief 查询当前是否已连接
 *
 * @return true  已连接
 * @return false 未连接
 */
bool wifi_manager_is_connected(void)
{
    return s_connected;
}

/**
 * @brief 获取当前 IP 字符串
 *
 * @return 内部静态 IP 缓冲区
 */
const char *wifi_manager_get_ip(void)
{
    return s_ip_str;
}
