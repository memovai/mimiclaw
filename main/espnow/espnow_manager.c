#include "espnow_manager.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

static const char *TAG = "espnow";

/* Chunk payload = 250 (ESP-NOW max) - 7 (header) */
#define ESPNOW_MAX_PAYLOAD    250
#define ESPNOW_HEADER_SIZE    7
#define ESPNOW_CHUNK_DATA     (ESPNOW_MAX_PAYLOAD - ESPNOW_HEADER_SIZE)

/* First chunk header: [type(1) | seq(2) | total_len(4)] */
/* Continuation:       [type(1) | seq(2) | padding(4)]   */

#define BIT_RESULT_DONE       BIT0
#define BIT_SEND_OK           BIT1
#define BIT_SEND_FAIL         BIT2

static bool s_initialized = false;
static uint8_t s_peer_mac[6] = {0};
static EventGroupHandle_t s_event_group = NULL;
static SemaphoreHandle_t s_mutex = NULL;

/* Result reassembly buffer */
static char *s_result_buf = NULL;
static size_t s_result_size = 0;
static size_t s_result_len = 0;
static uint32_t s_result_total = 0;
static uint16_t s_result_seq = 0;

static void build_header(uint8_t *buf, uint8_t type, uint16_t seq, uint32_t total_len)
{
    buf[0] = type;
    buf[1] = (uint8_t)(seq & 0xFF);
    buf[2] = (uint8_t)((seq >> 8) & 0xFF);
    buf[3] = (uint8_t)(total_len & 0xFF);
    buf[4] = (uint8_t)((total_len >> 8) & 0xFF);
    buf[5] = (uint8_t)((total_len >> 16) & 0xFF);
    buf[6] = (uint8_t)((total_len >> 24) & 0xFF);
}

static void on_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    if (s_event_group) {
        xEventGroupSetBits(s_event_group,
                           status == ESP_NOW_SEND_SUCCESS ? BIT_SEND_OK : BIT_SEND_FAIL);
    }
}

static void on_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < ESPNOW_HEADER_SIZE) return;

    uint8_t type = data[0];
    uint16_t seq = data[1] | (data[2] << 8);
    const uint8_t *payload = data + ESPNOW_HEADER_SIZE;
    int payload_len = len - ESPNOW_HEADER_SIZE;

    switch (type) {
    case ESPNOW_MSG_RESULT_START: {
        uint32_t total = data[3] | (data[4] << 8) | (data[5] << 16) | (data[6] << 24);
        s_result_total = total;
        s_result_len = 0;
        s_result_seq = 0;
        if (payload_len > 0 && s_result_buf && s_result_len + payload_len < s_result_size) {
            memcpy(s_result_buf + s_result_len, payload, payload_len);
            s_result_len += payload_len;
        }
        s_result_seq = seq + 1;
        ESP_LOGD(TAG, "Result start: total=%lu", (unsigned long)total);
        break;
    }
    case ESPNOW_MSG_RESULT_CHUNK:
        if (seq == s_result_seq && s_result_buf && payload_len > 0) {
            size_t copy = payload_len;
            if (s_result_len + copy >= s_result_size) {
                copy = s_result_size - s_result_len - 1;
            }
            if (copy > 0) {
                memcpy(s_result_buf + s_result_len, payload, copy);
                s_result_len += copy;
            }
            s_result_seq = seq + 1;
        }
        break;
    case ESPNOW_MSG_RESULT_END:
        if (s_result_buf && payload_len > 0) {
            size_t copy = payload_len;
            if (s_result_len + copy >= s_result_size) {
                copy = s_result_size - s_result_len - 1;
            }
            if (copy > 0) {
                memcpy(s_result_buf + s_result_len, payload, copy);
                s_result_len += copy;
            }
        }
        if (s_result_buf && s_result_len < s_result_size) {
            s_result_buf[s_result_len] = '\0';
        }
        ESP_LOGI(TAG, "Result complete: %u bytes", (unsigned)s_result_len);
        if (s_event_group) {
            xEventGroupSetBits(s_event_group, BIT_RESULT_DONE);
        }
        break;
    default:
        ESP_LOGD(TAG, "Ignoring msg type 0x%02x", type);
        break;
    }
}

static esp_err_t load_peer_mac(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(MIMI_NVS_ESPNOW, NVS_READONLY, &nvs);
    if (err != ESP_OK) return ESP_ERR_NOT_FOUND;

    size_t len = 6;
    err = nvs_get_blob(nvs, MIMI_NVS_KEY_PEER_MAC, s_peer_mac, &len);
    nvs_close(nvs);

    if (err != ESP_OK || len != 6) return ESP_ERR_NOT_FOUND;

    ESP_LOGI(TAG, "Peer MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             s_peer_mac[0], s_peer_mac[1], s_peer_mac[2],
             s_peer_mac[3], s_peer_mac[4], s_peer_mac[5]);
    return ESP_OK;
}

esp_err_t espnow_manager_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t err = load_peer_mac();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No ESP-NOW peer configured (use 'set espnow_peer XX:XX:XX:XX:XX:XX')");
        return ESP_ERR_NOT_FOUND;
    }

    s_event_group = xEventGroupCreate();
    s_mutex = xSemaphoreCreateMutex();
    if (!s_event_group || !s_mutex) return ESP_ERR_NO_MEM;

    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_now_register_send_cb(on_send_cb);
    esp_now_register_recv_cb(on_recv_cb);

    /* Add peer */
    esp_now_peer_info_t peer = {
        .channel = MIMI_ESPNOW_CHANNEL,
        .encrypt = false,
        .ifidx = WIFI_IF_STA,
    };
    memcpy(peer.peer_addr, s_peer_mac, 6);

    err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "esp_now_add_peer failed: %s", esp_err_to_name(err));
        esp_now_deinit();
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "ESP-NOW initialized");
    return ESP_OK;
}

bool espnow_is_ready(void)
{
    return s_initialized;
}

static esp_err_t send_chunk_and_wait(const uint8_t *data, size_t len)
{
    xEventGroupClearBits(s_event_group, BIT_SEND_OK | BIT_SEND_FAIL);

    esp_err_t err = esp_now_send(s_peer_mac, data, len);
    if (err != ESP_OK) return err;

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                            BIT_SEND_OK | BIT_SEND_FAIL,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(5000));
    if (bits & BIT_SEND_OK) return ESP_OK;
    return ESP_FAIL;
}

esp_err_t espnow_send_script(const char *script, char *result,
                              size_t result_size, int timeout_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (!script || !result || result_size == 0) return ESP_ERR_INVALID_ARG;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Set up result buffer */
    s_result_buf = result;
    s_result_size = result_size;
    s_result_len = 0;
    s_result_total = 0;
    s_result_seq = 0;
    result[0] = '\0';

    xEventGroupClearBits(s_event_group, BIT_RESULT_DONE);

    size_t script_len = strlen(script);
    size_t offset = 0;
    uint16_t seq = 0;
    uint8_t pkt[ESPNOW_MAX_PAYLOAD];
    esp_err_t err = ESP_OK;

    while (offset < script_len) {
        size_t remaining = script_len - offset;
        size_t chunk = (remaining > ESPNOW_CHUNK_DATA) ? ESPNOW_CHUNK_DATA : remaining;
        bool is_last = (offset + chunk >= script_len);

        uint8_t type;
        if (seq == 0) {
            type = ESPNOW_MSG_SCRIPT_START;
        } else if (is_last) {
            type = ESPNOW_MSG_SCRIPT_END;
        } else {
            type = ESPNOW_MSG_SCRIPT_CHUNK;
        }

        build_header(pkt, type, seq, (seq == 0) ? (uint32_t)script_len : 0);
        memcpy(pkt + ESPNOW_HEADER_SIZE, script + offset, chunk);

        err = send_chunk_and_wait(pkt, ESPNOW_HEADER_SIZE + chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Send chunk %u failed", seq);
            break;
        }

        offset += chunk;
        seq++;

        /* Small delay between chunks to avoid overwhelming receiver */
        if (!is_last) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

    if (err != ESP_OK) {
        s_result_buf = NULL;
        xSemaphoreGive(s_mutex);
        return err;
    }

    /* Wait for result from Board B */
    ESP_LOGI(TAG, "Script sent (%u bytes), waiting for result...", (unsigned)script_len);

    EventBits_t bits = xEventGroupWaitBits(s_event_group, BIT_RESULT_DONE,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(timeout_ms));

    s_result_buf = NULL;
    xSemaphoreGive(s_mutex);

    if (!(bits & BIT_RESULT_DONE)) {
        snprintf(result, result_size, "Error: Board B execution timed out after %d ms", timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
