#include "time_sync.h"
#include "mimi_config.h"

#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "time_sync";

#define TIME_SYNC_DONE_BIT  BIT0

static EventGroupHandle_t s_evt = NULL;
static bool s_started = false;

static void on_time_set(struct timeval *tv)
{
    time_t now = tv ? tv->tv_sec : time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char buf[40];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &local);
    ESP_LOGI(TAG, "System time synchronized: %s", buf);
    if (s_evt) {
        xEventGroupSetBits(s_evt, TIME_SYNC_DONE_BIT);
    }
}

esp_err_t time_sync_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    if (!s_evt) {
        s_evt = xEventGroupCreate();
        if (!s_evt) {
            return ESP_ERR_NO_MEM;
        }
    }

    setenv("TZ", MIMI_TIMEZONE, 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = true;
    config.sync_cb = on_time_set;
    config.smooth_sync = false;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "SNTP started (server=pool.ntp.org, tz=%s)", MIMI_TIMEZONE);
    s_started = true;
    return ESP_OK;
}

bool time_sync_wait(uint32_t timeout_ms)
{
    if (time_sync_is_set()) {
        return true;
    }
    if (!s_evt) {
        return false;
    }
    TickType_t ticks = (timeout_ms == portMAX_DELAY)
        ? portMAX_DELAY
        : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_evt, TIME_SYNC_DONE_BIT,
                                           pdFALSE, pdTRUE, ticks);
    return (bits & TIME_SYNC_DONE_BIT) != 0;
}

bool time_sync_is_set(void)
{
    time_t now = time(NULL);
    return now > 1700000000;  /* > Nov 2023, same threshold used elsewhere */
}
