#include "trigger/lick_trigger.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bus/message_bus.h"
#include "driver/touch_sensor_common.h"
#include "driver/touch_sensor_legacy.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu/QMI8658.h"
#include "sensors/env_sensor.h"

static const char *TAG = "lick_trigger";

static TaskHandle_t s_lick_task = NULL;
static const touch_pad_t s_touch_channel = TOUCH_PAD_NUM1;  // GPIO1
static bool s_touch_last_active = false;
static int64_t s_last_trigger_us = 0;

#define LICK_TOUCH_ENTER_RATIO_PCT   78
#define LICK_TOUCH_RELEASE_RATIO_PCT 84
#define LICK_TOUCH_COOLDOWN_US       (1500LL * 1000)

static esp_err_t lick_touch_init(void)
{
    ESP_RETURN_ON_ERROR(touch_pad_init(), TAG, "touch init failed");
    ESP_RETURN_ON_ERROR(touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER), TAG, "set FSM mode failed");
    ESP_RETURN_ON_ERROR(touch_pad_config(s_touch_channel), TAG, "touch channel config failed");
    ESP_RETURN_ON_ERROR(touch_pad_set_charge_discharge_times(TOUCH_PAD_MEASURE_CYCLE_DEFAULT),
                        TAG, "set charge/discharge failed");
    ESP_RETURN_ON_ERROR(touch_pad_set_measurement_interval(TOUCH_PAD_SLEEP_CYCLE_DEFAULT),
                        TAG, "set interval failed");

    touch_filter_config_t filter_cfg = {
        .mode = TOUCH_PAD_FILTER_IIR_16,
        .debounce_cnt = 1,
        .noise_thr = 1,
        .jitter_step = 4,
        .smh_lvl = TOUCH_PAD_SMOOTH_IIR_2,
    };
    ESP_RETURN_ON_ERROR(touch_pad_filter_set_config(&filter_cfg), TAG, "filter config failed");
    ESP_RETURN_ON_ERROR(touch_pad_filter_enable(), TAG, "filter enable failed");
    ESP_RETURN_ON_ERROR(touch_pad_fsm_start(), TAG, "touch FSM start failed");

    // Give filter/benchmark a short warmup window.
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "Touch channel enabled: TOUCH_PAD_NUM%d (GPIO%d)",
             (int)s_touch_channel, (int)s_touch_channel);
    return ESP_OK;
}

static bool lick_touch_is_active(void)
{
    uint32_t smooth = 0;
    uint32_t benchmark = 0;

    if (touch_pad_filter_read_smooth(s_touch_channel, &smooth) != ESP_OK) {
        return false;
    }
    if (touch_pad_read_benchmark(s_touch_channel, &benchmark) != ESP_OK || benchmark == 0) {
        return false;
    }

    uint32_t enter_threshold = (benchmark * LICK_TOUCH_ENTER_RATIO_PCT) / 100;
    uint32_t release_threshold = (benchmark * LICK_TOUCH_RELEASE_RATIO_PCT) / 100;

    if (s_touch_last_active) {
        return smooth < release_threshold;
    }
    return smooth < enter_threshold;
}

static void route_target(char *channel, size_t channel_size, char *chat_id, size_t chat_id_size)
{
    if (message_bus_get_latest_client_context(channel, channel_size, chat_id, chat_id_size) == ESP_OK) {
        return;
    }

    strncpy(channel, MIMI_CHAN_SYSTEM, channel_size - 1);
    channel[channel_size - 1] = '\0';
    strncpy(chat_id, "lick", chat_id_size - 1);
    chat_id[chat_id_size - 1] = '\0';
}

static void enqueue_lick_prompt(void)
{
    float temp_c = 0.0f;
    float hum_pct = 0.0f;
    esp_err_t env_err = env_sensor_read(&temp_c, &hum_pct);
    bool humidity_ok = (env_err == ESP_OK);

    if (!humidity_ok) {
        esp_err_t temp_err = QMI8658_Read_Temperature(&temp_c);
        if (temp_err != ESP_OK) {
            ESP_LOGW(TAG, "Fallback temperature read failed: %s", esp_err_to_name(temp_err));
            temp_c = 0.0f;
        }
    }

    char channel[16] = {0};
    char chat_id[32] = {0};
    route_target(channel, sizeof(channel), chat_id, sizeof(chat_id));

    char text[512];
    if (humidity_ok) {
        snprintf(
            text, sizeof(text),
            "Hardware event: tongue switch activated.\n"
            "Measured data:\n"
            "- temperature_c: %.2f\n"
            "- humidity_pct: %.2f\n"
            "Please summarize this state and recommend the next step in concise bullets.",
            temp_c, hum_pct);
    } else {
        snprintf(
            text, sizeof(text),
            "Hardware event: tongue switch activated.\n"
            "Measured data:\n"
            "- temperature_c: %.2f\n"
            "- humidity_pct: unavailable (%s)\n"
            "Please summarize the state and recommend the next step, including sensor troubleshooting guidance.",
            temp_c, esp_err_to_name(env_err));
    }

    mimi_msg_t msg = {0};
    strncpy(msg.channel, channel, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
    msg.content = strdup(text);
    if (!msg.content) {
        return;
    }

    if (message_bus_push_inbound(&msg) != ESP_OK) {
        ESP_LOGW(TAG, "Inbound queue full, drop lick event prompt");
        free(msg.content);
        return;
    }

    ESP_LOGI(TAG, "Lick event pushed to agent via %s:%s", msg.channel, msg.chat_id);
}

static void lick_trigger_task(void *arg)
{
    (void)arg;

    if (lick_touch_init() != ESP_OK) {
        ESP_LOGE(TAG, "Touch init failed, lick trigger task stopped");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        bool active = lick_touch_is_active();
        int64_t now = esp_timer_get_time();

        if (active && !s_touch_last_active && (now - s_last_trigger_us > LICK_TOUCH_COOLDOWN_US)) {
            s_last_trigger_us = now;
            enqueue_lick_prompt();
        }
        s_touch_last_active = active;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t lick_trigger_init(void)
{
    if (s_lick_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        lick_trigger_task, "lick_trigger",
        4096, NULL, 4, &s_lick_task, 0);
    if (ok != pdPASS) {
        s_lick_task = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Lick trigger initialized (touch electrode -> env sample -> agent)");
    return ESP_OK;
}
