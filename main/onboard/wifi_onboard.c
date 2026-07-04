#include "wifi_onboard.h"
#include "onboard_html.h"
#include "mimi_config.h"
#include "wifi/wifi_manager.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "onboard";
static httpd_handle_t s_server = NULL;
static bool s_captive_mode = false;

static void json_add_effective_config(cJSON *root, const char *json_key,
                                      const char *ns, const char *nvs_key,
                                      const char *build_val)
{
    char value[256] = {0};
    bool found = false;

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(value);
        if (nvs_get_str(nvs, nvs_key, value, &len) == ESP_OK) {
            found = true;
        }
        nvs_close(nvs);
    }

    if (!found && build_val) {
        strlcpy(value, build_val, sizeof(value));
    }

    cJSON_AddStringToObject(root, json_key, value);
}

static void json_add_effective_config_u16(cJSON *root, const char *json_key,
                                          const char *ns, const char *nvs_key,
                                          const char *build_val)
{
    char value[16] = {0};
    bool found = false;

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        uint16_t port = 0;
        if (nvs_get_u16(nvs, nvs_key, &port) == ESP_OK && port > 0) {
            snprintf(value, sizeof(value), "%u", (unsigned)port);
            found = true;
        }
        nvs_close(nvs);
    }

    if (!found && build_val) {
        strlcpy(value, build_val, sizeof(value));
    }

    cJSON_AddStringToObject(root, json_key, value);
}

/* ── DNS hijack ─────────────────────────────────────────────────── */

/* Minimal DNS response: always answer 192.168.4.1 */
static void dns_hijack_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket error");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS hijack listening on :53");

    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t client_len;

    while (1) {
        client_len = sizeof(client);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &client_len);
        if (len < 12) continue;  /* too short for DNS header */

        /* Build response: copy query, set response flags, append answer */
        uint8_t resp[512];
        if (len + 16 > (int)sizeof(resp)) continue;

        memcpy(resp, buf, len);

        /* Set QR=1 (response), AA=1 (authoritative), RA=1 */
        resp[2] = 0x85;  /* QR=1, Opcode=0, AA=1, TC=0, RD=1 */
        resp[3] = 0x80;  /* RA=1, Z=0, RCODE=0 (no error) */

        /* Answer count = 1 */
        resp[6] = 0x00;
        resp[7] = 0x01;

        /* Append answer: pointer to name + A record with 192.168.4.1 */
        int off = len;
        resp[off++] = 0xC0;  /* pointer */
        resp[off++] = 0x0C;  /* offset to question name */
        resp[off++] = 0x00; resp[off++] = 0x01;  /* type A */
        resp[off++] = 0x00; resp[off++] = 0x01;  /* class IN */
        resp[off++] = 0x00; resp[off++] = 0x00;
        resp[off++] = 0x00; resp[off++] = 0x3C;  /* TTL = 60 */
        resp[off++] = 0x00; resp[off++] = 0x04;  /* data length = 4 */
        resp[off++] = 192; resp[off++] = 168;
        resp[off++] = 4;   resp[off++] = 1;

        sendto(sock, resp, off, 0,
               (struct sockaddr *)&client, client_len);
    }
}

/* ── HTTP handlers ──────────────────────────────────────────────── */

static esp_err_t http_get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, ONBOARD_HTML, sizeof(ONBOARD_HTML) - 1);
}

/* Captive portal detection endpoints → redirect to root */
static esp_err_t http_captive_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t http_get_scan(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ESP_FAIL;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > MIMI_ONBOARD_MAX_SCAN) ap_count = MIMI_ONBOARD_MAX_SCAN;

    wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_list) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    uint16_t ap_max = ap_count;
    esp_wifi_scan_get_ap_records(&ap_max, ap_list);

    cJSON *arr = cJSON_CreateArray();
    for (uint16_t i = 0; i < ap_max; i++) {
        if (ap_list[i].ssid[0] == '\0') continue;  /* skip hidden */
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "ssid", (const char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(obj, "rssi", ap_list[i].rssi);
        cJSON_AddNumberToObject(obj, "ch", ap_list[i].primary);
        cJSON_AddBoolToObject(obj, "auth", ap_list[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, obj);
    }
    free(ap_list);

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

static esp_err_t http_get_config(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    json_add_effective_config(root, "ssid", MIMI_NVS_WIFI, MIMI_NVS_KEY_SSID, MIMI_SECRET_WIFI_SSID);
    json_add_effective_config(root, "password", MIMI_NVS_WIFI, MIMI_NVS_KEY_PASS, MIMI_SECRET_WIFI_PASS);
    json_add_effective_config(root, "api_key", MIMI_NVS_LLM, MIMI_NVS_KEY_API_KEY, MIMI_SECRET_API_KEY);
    json_add_effective_config(root, "model", MIMI_NVS_LLM, MIMI_NVS_KEY_MODEL, MIMI_SECRET_MODEL);
    json_add_effective_config(root, "provider", MIMI_NVS_LLM, MIMI_NVS_KEY_PROVIDER, MIMI_SECRET_MODEL_PROVIDER);
    json_add_effective_config(root, "tg_token", MIMI_NVS_TG, MIMI_NVS_KEY_TG_TOKEN, MIMI_SECRET_TG_TOKEN);
    json_add_effective_config(root, "groq_key", MIMI_NVS_TG, MIMI_NVS_KEY_GROQ_KEY, MIMI_SECRET_GROQ_KEY);
    json_add_effective_config(root, "feishu_app_id", MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_ID, MIMI_SECRET_FEISHU_APP_ID);
    json_add_effective_config(root, "feishu_app_secret", MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_SECRET, MIMI_SECRET_FEISHU_APP_SECRET);
    json_add_effective_config(root, "proxy_host", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_HOST, MIMI_SECRET_PROXY_HOST);
    json_add_effective_config_u16(root, "proxy_port", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_PORT, MIMI_SECRET_PROXY_PORT);
    json_add_effective_config(root, "proxy_type", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_TYPE, MIMI_SECRET_PROXY_TYPE);
    json_add_effective_config(root, "search_key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_API_KEY, MIMI_SECRET_SEARCH_KEY);
    json_add_effective_config(root, "tavily_key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_TAVILY_KEY, MIMI_SECRET_TAVILY_KEY);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t ret = httpd_resp_send(req, json, strlen(json));
    free(json);
    return ret;
}

/*
 * Sync one JSON string field into NVS.
 * - missing field: leave current NVS value unchanged
 * - empty string: erase current NVS value
 * - non-empty string: save/update current NVS value
 */
static void nvs_sync_field(cJSON *root, const char *json_key,
                           const char *ns, const char *nvs_key)
{
    cJSON *item = cJSON_GetObjectItem(root, json_key);
    if (!item || !cJSON_IsString(item)) return;

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READWRITE, &nvs) == ESP_OK) {
        if (item->valuestring[0] == '\0') {
            esp_err_t err = nvs_erase_key(nvs, nvs_key);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Cleared %s/%s", ns, nvs_key);
            } else if (err != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "Failed clearing %s/%s: %s", ns, nvs_key, esp_err_to_name(err));
            }
        } else {
            nvs_set_str(nvs, nvs_key, item->valuestring);
            ESP_LOGI(TAG, "Saved %s/%s", ns, nvs_key);
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static void nvs_sync_u16_field(cJSON *root, const char *json_key,
                               const char *ns, const char *nvs_key)
{
    cJSON *item = cJSON_GetObjectItem(root, json_key);
    if (!item || !cJSON_IsString(item)) return;

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READWRITE, &nvs) == ESP_OK) {
        if (item->valuestring[0] == '\0') {
            esp_err_t err = nvs_erase_key(nvs, nvs_key);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Cleared %s/%s", ns, nvs_key);
            } else if (err != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "Failed clearing %s/%s: %s", ns, nvs_key, esp_err_to_name(err));
            }
        } else {
            char *end = NULL;
            unsigned long value = strtoul(item->valuestring, &end, 10);
            if (end == item->valuestring || *end != '\0' || value > UINT16_MAX) {
                ESP_LOGW(TAG, "Ignoring invalid %s value: %s", json_key, item->valuestring);
            } else {
                ESP_ERROR_CHECK(nvs_set_u16(nvs, nvs_key, (uint16_t)value));
                ESP_LOGI(TAG, "Saved %s/%s", ns, nvs_key);
            }
        }
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static esp_err_t http_post_save(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad length");
        return ESP_FAIL;
    }

    char *buf = calloc(1, total_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv error");
            return ESP_FAIL;
        }
        received += ret;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    /* WiFi (required) */
    nvs_sync_field(root, "ssid",     MIMI_NVS_WIFI,   MIMI_NVS_KEY_SSID);
    nvs_sync_field(root, "password", MIMI_NVS_WIFI,   MIMI_NVS_KEY_PASS);

    /* LLM */
    nvs_sync_field(root, "api_key",  MIMI_NVS_LLM,    MIMI_NVS_KEY_API_KEY);
    nvs_sync_field(root, "model",    MIMI_NVS_LLM,    MIMI_NVS_KEY_MODEL);
    nvs_sync_field(root, "provider", MIMI_NVS_LLM,    MIMI_NVS_KEY_PROVIDER);

    /* Telegram */
    nvs_sync_field(root, "tg_token", MIMI_NVS_TG,     MIMI_NVS_KEY_TG_TOKEN);
    nvs_sync_field(root, "groq_key", MIMI_NVS_TG,     MIMI_NVS_KEY_GROQ_KEY);

    /* Feishu */
    nvs_sync_field(root, "feishu_app_id",     MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_ID);
    nvs_sync_field(root, "feishu_app_secret", MIMI_NVS_FEISHU, MIMI_NVS_KEY_FEISHU_APP_SECRET);

    /* Proxy */
    nvs_sync_field(root, "proxy_host", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_HOST);
    nvs_sync_u16_field(root, "proxy_port", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_PORT);
    nvs_sync_field(root, "proxy_type", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_TYPE);

    /* Search */
    nvs_sync_field(root, "search_key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_API_KEY);
    nvs_sync_field(root, "tavily_key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_TAVILY_KEY);

    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", 11);

    ESP_LOGI(TAG, "Configuration saved, restarting in 2s...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

    return ESP_OK;  /* unreachable */
}

/* ── Soft AP + HTTP server startup ──────────────────────────────── */

static esp_err_t start_softap(bool keep_sta)
{
    /* Get last 2 bytes of MAC for unique SSID suffix */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "%s%02X%02X", MIMI_ONBOARD_AP_PREFIX, mac[4], mac[5]);

    /* Create AP netif if not already present */
    static esp_netif_t *ap_netif = NULL;
    if (!ap_netif) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }

    /* APSTA lets the local config AP coexist with WiFi scanning/STA usage. */
    (void)keep_sta;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .channel = 1,
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && !(keep_sta && err == ESP_ERR_WIFI_CONN)) {
        return err;
    }

    ESP_LOGI(TAG, "Soft AP started: %s (open)", ssid);
    return ESP_OK;
}

static httpd_handle_t start_http_server(bool captive)
{
    if (s_server) {
        if (captive && !s_captive_mode) {
            ESP_LOGW(TAG, "HTTP server already running without captive redirects");
        }
        return s_server;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = MIMI_ONBOARD_HTTP_PORT;
    config.max_uri_handlers = captive ? 16 : 8;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    /* Main page */
    httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = http_get_root,
    };
    httpd_register_uri_handler(s_server, &uri_root);

    httpd_uri_t uri_config = {
        .uri = "/config", .method = HTTP_GET, .handler = http_get_config,
    };
    httpd_register_uri_handler(s_server, &uri_config);

    /* WiFi scan */
    httpd_uri_t uri_scan = {
        .uri = "/scan", .method = HTTP_GET, .handler = http_get_scan,
    };
    httpd_register_uri_handler(s_server, &uri_scan);

    /* Save config */
    httpd_uri_t uri_save = {
        .uri = "/save", .method = HTTP_POST, .handler = http_post_save,
    };
    httpd_register_uri_handler(s_server, &uri_save);

    if (captive) {
        /* Captive portal detection endpoints */
        const char *captive_uris[] = {
            "/generate_204",           /* Android */
            "/gen_204",                /* Android alt */
            "/hotspot-detect.html",    /* iOS/macOS */
            "/library/test/success.html", /* iOS alt */
            "/connecttest.txt",        /* Windows */
            "/redirect",               /* Windows alt */
        };
        for (int i = 0; i < sizeof(captive_uris) / sizeof(captive_uris[0]); i++) {
            httpd_uri_t uri_captive = {
                .uri = captive_uris[i],
                .method = HTTP_GET,
                .handler = http_captive_redirect,
            };
            httpd_register_uri_handler(s_server, &uri_captive);
        }
    }

    s_captive_mode = captive;
    ESP_LOGI(TAG, "HTTP server started on port %d", MIMI_ONBOARD_HTTP_PORT);
    return s_server;
}

/* ── Public API ─────────────────────────────────────────────────── */

esp_err_t wifi_onboard_start(wifi_onboard_mode_t mode)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Starting WiFi Configuration Portal");
    ESP_LOGI(TAG, "========================================");

    bool captive = (mode == WIFI_ONBOARD_MODE_CAPTIVE);
    if (captive) {
        /* Stop STA retries before starting captive portal. */
        wifi_manager_set_reconnect_enabled(false);
        wifi_manager_stop();
    }

    /* Start soft AP */
    esp_err_t err = start_softap(!captive);
    if (err != ESP_OK) return err;

    if (captive) {
        /* Start DNS hijack only for true captive portal mode. */
        xTaskCreate(dns_hijack_task, "dns_hijack",
                    MIMI_ONBOARD_DNS_STACK, NULL, 5, NULL);
    }

    /* Start HTTP server */
    httpd_handle_t server = start_http_server(captive);
    if (!server) return ESP_FAIL;

    ESP_LOGI(TAG, "Connect to MimiClaw-XXXX WiFi, then open http://192.168.4.1");

    if (!captive) {
        ESP_LOGI(TAG, "Local admin portal stays available while STA is connected");
        return ESP_OK;
    }

    /* Block forever — onboarding ends with esp_restart() in /save handler */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    return ESP_OK;  /* unreachable */
}
