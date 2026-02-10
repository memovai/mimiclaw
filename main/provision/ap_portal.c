#include "provision/ap_portal.h"
#include "mimi_config.h"
#include "wifi/wifi_manager.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ap_portal";

static httpd_handle_t s_httpd = NULL;
static bool s_ap_netif_created = false;

static const char *HTML_PAGE =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>MimiClaw Setup</title>"
"<style>"
":root{--bg:#f6efe4;--paper:#fff8f0;--ink:#1d1b18;--accent:#d84f2a;--accent2:#0f7a68;--line:#e7d9c8;}"
"*{box-sizing:border-box}body{margin:0;font-family:'Avenir Next','Trebuchet MS','Segoe UI',sans-serif;background:"
"radial-gradient(1200px 500px at -10% -10%,#ffd6c7 0,transparent 55%),"
"radial-gradient(900px 450px at 110% 120%,#c7f0e8 0,transparent 55%),var(--bg);color:var(--ink);min-height:100vh;display:grid;place-items:center;padding:20px}"
".card{width:min(760px,100%);background:linear-gradient(160deg,#fffefc 0,#fff7ee 100%);border:1px solid var(--line);border-radius:22px;padding:28px;box-shadow:0 18px 45px rgba(71,39,24,.14)}"
".title{font:800 clamp(30px,6vw,48px)/1.02 'Trebuchet MS','Avenir Next',sans-serif;margin:0;letter-spacing:.3px}"
".subtitle{margin:10px 0 24px;color:#6a5d50;font-size:15px}"
".badge{display:inline-block;border:1px solid #f1baa9;background:#fff1ec;color:#9d371c;border-radius:999px;padding:6px 12px;font-size:12px;font-weight:700;letter-spacing:.3px;text-transform:uppercase}"
".grid{display:grid;grid-template-columns:1fr 1fr;gap:14px}.full{grid-column:1/-1}"
"label{display:block;font-size:12px;font-weight:700;color:#594f45;margin:0 0 7px;text-transform:uppercase;letter-spacing:.35px}"
"input,select{width:100%;border:1px solid var(--line);background:#fff;border-radius:12px;padding:13px 12px;font-size:15px;outline:none}"
"input:focus,select:focus{border-color:var(--accent2);box-shadow:0 0 0 3px rgba(15,122,104,.12)}"
".actions{display:flex;flex-wrap:wrap;gap:10px;margin-top:16px}"
"button{border:0;border-radius:12px;padding:12px 16px;font-weight:800;cursor:pointer;transition:.2s transform,.2s box-shadow}"
".primary{background:linear-gradient(120deg,var(--accent),#f07a45);color:#fff;box-shadow:0 10px 18px rgba(216,79,42,.25)}"
".ghost{background:#fff;color:#4a4239;border:1px solid var(--line)}"
"button:hover{transform:translateY(-1px)}"
".note{margin-top:14px;font-size:13px;color:#6a5d50}"
".ok{margin-top:14px;padding:12px;border-radius:10px;background:#ecfff9;border:1px solid #bfeedd;color:#0f5f4b;display:none}"
"@media (max-width:640px){.grid{grid-template-columns:1fr}.card{padding:20px}}"
"</style></head><body>"
"<div class='card'>"
"<span class='badge'>MimiClaw AP Provisioning</span>"
"<h1 class='title'>Connect MimiClaw</h1>"
"<p class='subtitle'>Select your WiFi and save credentials. Device will reboot automatically.</p>"
"<form id='f' method='POST' action='/save'>"
"<div class='grid'>"
"<div class='full'><label for='ssid_pick'>Nearby Networks</label><select id='ssid_pick'><option value=''>-- Select scanned SSID --</option>{{SSID_OPTIONS}}</select></div>"
"<div class='full'><label for='ssid'>WiFi SSID</label><input id='ssid' name='ssid' required maxlength='32' placeholder='Your WiFi name'></div>"
"<div class='full'><label for='password'>WiFi Password</label><input id='password' name='password' type='password' maxlength='64' placeholder='Your WiFi password'></div>"
"</div>"
"<div class='actions'><button class='primary' type='submit'>Save & Reboot</button><button class='ghost' type='button' onclick='location.reload()'>Rescan</button></div>"
"<p class='note'>Tip: connect your phone/laptop to AP <b>" MIMI_AP_PROV_SSID "</b> first, then open <b>http://192.168.4.1</b>.</p>"
"<div class='ok' id='ok'>Credentials saved. Rebooting now...</div>"
"</form></div>"
"<script>"
"const pick=document.getElementById('ssid_pick');const ssid=document.getElementById('ssid');"
"pick.addEventListener('change',()=>{if(pick.value)ssid.value=pick.value;});"
"document.getElementById('f').addEventListener('submit',()=>{const ok=document.getElementById('ok');ok.style.display='block';});"
"</script></body></html>";

static void url_decode(char *dst, size_t dst_size, const char *src)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 1 < dst_size; si++) {
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else if (src[si] == '%' && isxdigit((unsigned char)src[si + 1]) && isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = { src[si + 1], src[si + 2], '\0' };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static void build_ssid_options(char *out, size_t out_size)
{
    out[0] = '\0';
    wifi_scan_config_t scan_cfg = {0};
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) return;

    uint16_t ap_num = 0;
    if (esp_wifi_scan_get_ap_num(&ap_num) != ESP_OK || ap_num == 0) return;
    if (ap_num > 16) ap_num = 16;

    wifi_ap_record_t aps[16];
    memset(aps, 0, sizeof(aps));
    if (esp_wifi_scan_get_ap_records(&ap_num, aps) != ESP_OK) return;

    size_t off = 0;
    for (int i = 0; i < ap_num; i++) {
        const char *ssid = (const char *)aps[i].ssid;
        if (!ssid[0]) continue;
        off += snprintf(out + off, out_size - off,
                        "<option value='%s'>%s (%d dBm)</option>",
                        ssid, ssid, (int)aps[i].rssi);
        if (off >= out_size - 1) break;
    }
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1800));
    esp_restart();
}

static esp_err_t handle_root(httpd_req_t *req)
{
    char *options = calloc(1, 2048);
    if (!options) return httpd_resp_send_500(req);
    build_ssid_options(options, 2048);

    const char *marker = "{{SSID_OPTIONS}}";
    const char *at = strstr(HTML_PAGE, marker);
    if (!at) {
        free(options);
        return httpd_resp_send_500(req);
    }

    size_t prefix = (size_t)(at - HTML_PAGE);
    size_t suffix_len = strlen(at + strlen(marker));
    size_t page_cap = prefix + strlen(options) + suffix_len + 1;
    char *page = malloc(page_cap);
    if (!page) {
        free(options);
        return httpd_resp_send_500(req);
    }
    int n = snprintf(page, page_cap, "%.*s%s%s",
                     (int)prefix, HTML_PAGE, options, at + strlen(marker));
    free(options);
    if (n <= 0 || (size_t)n >= page_cap) {
        free(page);
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return ret;
}

static esp_err_t handle_save(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 512) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid payload");
    }

    char body[513];
    int read = httpd_req_recv(req, body, req->content_len);
    if (read <= 0) return ESP_FAIL;
    body[read] = '\0';

    char ssid[64] = {0};
    char pass[96] = {0};

    char *ssid_raw = strstr(body, "ssid=");
    char *pass_raw = strstr(body, "password=");

    if (ssid_raw) {
        ssid_raw += 5;
        char *end = strchr(ssid_raw, '&');
        char tmp[96];
        size_t len = end ? (size_t)(end - ssid_raw) : strlen(ssid_raw);
        if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
        memcpy(tmp, ssid_raw, len);
        tmp[len] = '\0';
        url_decode(ssid, sizeof(ssid), tmp);
    }
    if (pass_raw) {
        pass_raw += 9;
        char *end = strchr(pass_raw, '&');
        char tmp[128];
        size_t len = end ? (size_t)(end - pass_raw) : strlen(pass_raw);
        if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
        memcpy(tmp, pass_raw, len);
        tmp[len] = '\0';
        url_decode(pass, sizeof(pass), tmp);
    }

    if (!ssid[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
    }

    ESP_LOGI(TAG, "Provisioning save: ssid=%s pass_len=%d", ssid, (int)strlen(pass));
    if (wifi_manager_set_credentials(ssid, pass) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<html><body style='font-family:sans-serif;padding:24px'>"
        "<h2>Saved successfully</h2><p>MimiClaw will reboot in 2 seconds...</p>"
        "</body></html>");

    xTaskCreate(restart_task, "prov_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t ap_portal_start(void)
{
    if (!s_ap_netif_created) {
        esp_netif_create_default_wifi_ap();
        s_ap_netif_created = true;
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_APSTA) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    }

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, MIMI_AP_PROV_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(MIMI_AP_PROV_SSID);
    ap_cfg.ap.channel = MIMI_AP_PROV_CHANNEL;
    ap_cfg.ap.max_connection = MIMI_AP_PROV_MAX_CONN;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK && start_err != ESP_ERR_WIFI_CONN) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(start_err));
        return start_err;
    }

    if (s_httpd) {
        ESP_LOGW(TAG, "AP portal already running");
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32768;
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;

    ESP_ERROR_CHECK(httpd_start(&s_httpd, &cfg));

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handle_root,
    };
    httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = handle_save,
    };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);

    ESP_LOGI(TAG, "AP provisioning started: SSID=%s, open http://192.168.4.1", MIMI_AP_PROV_SSID);
    return ESP_OK;
}
