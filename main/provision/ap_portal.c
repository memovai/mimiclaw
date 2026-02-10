#include "provision/ap_portal.h"
#include "mimi_config.h"
#include "wifi/wifi_manager.h"
#include "telegram/telegram_bot.h"
#include "llm/llm_proxy.h"
#include "tools/tool_web_search.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "ap_portal";

static httpd_handle_t s_httpd = NULL;
static bool s_ap_netif_created = false;

typedef struct {
    bool wifi_pass_set;
    bool tg_token_set;
    bool llm_api_key_set;
    bool search_key_set;
    bool proxy_enabled;
    char wifi_ssid[33];
    char llm_model[64];
    char llm_base_url[256];
    char proxy_host[64];
    uint16_t proxy_port;
} portal_status_t;

static const char *HTML_PAGE =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>MimiClaw Setup</title>"
"<link rel='icon' href='data:,'>"
"<style>"
":root{--bg:#f6efe4;--paper:#fff8f0;--ink:#1d1b18;--muted:#6a5d50;--accent:#d84f2a;--accent2:#0f7a68;--line:#e7d9c8;--ok:#0f5f4b;--okbg:#ecfff9;--err:#8f2712;--errbg:#ffefea;}"
"*{box-sizing:border-box}"
"body{margin:0;font-family:'Avenir Next','Trebuchet MS','Segoe UI',sans-serif;background:"
"radial-gradient(1200px 500px at -10% -10%,#ffd6c7 0,transparent 55%),"
"radial-gradient(900px 450px at 110% 120%,#c7f0e8 0,transparent 55%),var(--bg);"
"color:var(--ink);min-height:100vh;display:grid;place-items:center;padding:16px}"
".card{width:min(860px,100%);background:linear-gradient(160deg,#fffefc 0,#fff7ee 100%);border:1px solid var(--line);border-radius:22px;padding:22px;box-shadow:0 18px 45px rgba(71,39,24,.14)}"
".badge{display:inline-block;border:1px solid #f1baa9;background:#fff1ec;color:#9d371c;border-radius:999px;padding:6px 12px;font-size:12px;font-weight:700;letter-spacing:.3px;text-transform:uppercase}"
"h1{font:800 clamp(28px,5vw,44px)/1.04 'Trebuchet MS','Avenir Next',sans-serif;margin:10px 0 6px;letter-spacing:.2px}"
".sub{margin:0 0 16px;color:var(--muted);font-size:14px}"
".steps{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 18px}"
".step{background:#fff;border:1px solid var(--line);border-radius:999px;padding:6px 10px;font-size:12px;color:#5f5449}"
".section{border:1px solid var(--line);border-radius:16px;padding:14px;background:rgba(255,255,255,.7);margin:0 0 12px}"
".section h2{margin:0 0 10px;font-size:15px;letter-spacing:.2px}"
".grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}"
".full{grid-column:1/-1}"
"label{display:flex;align-items:center;justify-content:space-between;gap:8px;font-size:12px;font-weight:700;color:#594f45;margin:0 0 6px;text-transform:uppercase;letter-spacing:.3px}"
".state{font-weight:700;font-size:11px;padding:2px 8px;border-radius:999px;border:1px solid var(--line);background:#fff;color:#7b6c5c}"
".state.on{border-color:#9ddfcb;background:#effdf8;color:#0f5f4b}"
"input,select{width:100%;height:44px;border:1px solid var(--line);background:#fff;border-radius:12px;padding:0 12px;font-size:15px;outline:none}"
"input:focus-visible,select:focus-visible{border-color:var(--accent2);box-shadow:0 0 0 3px rgba(15,122,104,.14)}"
"details{border:1px dashed #d9c8b4;border-radius:12px;padding:10px;background:#fffdf8}"
"summary{cursor:pointer;font-weight:700;color:#4a4239}"
".check{display:flex;align-items:center;gap:8px;margin:8px 0 0;color:#544a40;font-size:13px}"
".check input{width:18px;height:18px;padding:0}"
".actions{display:flex;flex-wrap:wrap;gap:10px;margin-top:14px}"
"button{border:0;border-radius:12px;padding:12px 16px;font-weight:800;cursor:pointer;transition:transform .2s ease,box-shadow .2s ease}"
".primary{background:linear-gradient(120deg,var(--accent),#f07a45);color:#fff;box-shadow:0 10px 18px rgba(216,79,42,.25)}"
".ghost{background:#fff;color:#4a4239;border:1px solid var(--line)}"
"button:hover{transform:translateY(-1px)}"
"button:disabled{opacity:.6;cursor:not-allowed;transform:none}"
".msg{display:none;margin-top:12px;padding:11px 12px;border-radius:10px;font-size:14px}"
".msg.ok{display:block;background:var(--okbg);border:1px solid #bfeedd;color:var(--ok)}"
".msg.err{display:block;background:var(--errbg);border:1px solid #ffc8bb;color:var(--err)}"
".note{margin-top:12px;font-size:12px;color:#6a5d50}"
"@media (max-width:760px){.grid{grid-template-columns:1fr}.card{padding:16px}}"
"@media (prefers-reduced-motion: reduce){*{animation:none !important;transition:none !important}}"
"</style></head><body>"
"<div class='card'>"
"<span class='badge'>MimiClaw Setup Portal</span>"
"<h1>Configure MimiClaw</h1>"
"<p class='sub'>Set WiFi and optional runtime parameters in one place. Save applies configuration and reboots the device.</p>"
"<div class='steps'><span class='step'>1. Connect AP: " MIMI_AP_PROV_SSID "</span><span class='step'>2. Open 192.168.4.1</span><span class='step'>3. Save & Reboot</span></div>"
"<form id='cfg' method='POST' action='/save'>"
"<section class='section'>"
"<h2>Network</h2>"
"<div class='grid'>"
"<div class='full'><label for='ssid_pick'>Nearby Networks <span class='state' id='wifi_state'>unknown</span></label><select id='ssid_pick'><option value=''>-- Select scanned SSID --</option>{{SSID_OPTIONS}}</select></div>"
"<div class='full'><label for='ssid'>WiFi SSID</label><input id='ssid' name='ssid' required maxlength='32' placeholder='Your WiFi name'></div>"
"<div class='full'><label for='password'>WiFi Password</label><input id='password' name='password' type='password' maxlength='64' placeholder='Leave empty for open network'></div>"
"</div></section>"
"<section class='section'>"
"<h2>AI</h2>"
"<div class='grid'>"
"<div class='full'><label for='model'>Model</label><input id='model' name='model' maxlength='63' placeholder='claude-sonnet-4-5-20250929'></div>"
"<div class='full'><label for='base_url'>LLM Base URL</label><input id='base_url' name='base_url' maxlength='255' placeholder='https://api.anthropic.com/v1/messages'></div>"
"<div class='full'><label for='llm_api_key'>LLM API Key <span class='state' id='llm_key_state'>unknown</span></label><input id='llm_api_key' name='llm_api_key' type='password' maxlength='127' placeholder='Leave empty to keep current'></div>"
"</div></section>"
"<section class='section'>"
"<h2>Messaging & Tools</h2>"
"<div class='grid'>"
"<div class='full'><label for='tg_token'>Telegram Bot Token <span class='state' id='tg_state'>unknown</span></label><input id='tg_token' name='tg_token' type='password' maxlength='127' placeholder='Leave empty to keep current'></div>"
"<div class='full'><label for='search_key'>Search API Key <span class='state' id='search_state'>unknown</span></label><input id='search_key' name='search_key' type='password' maxlength='127' placeholder='Leave empty to keep current'></div>"
"</div></section>"
"<section class='section'>"
"<h2>Proxy</h2>"
"<details><summary>Advanced proxy settings</summary>"
"<label class='check' for='proxy_enabled'><input id='proxy_enabled' name='proxy_enabled' type='checkbox' value='1'>Enable HTTP CONNECT proxy</label>"
"<div class='grid'>"
"<div><label for='proxy_host'>Proxy Host <span class='state' id='proxy_state'>unknown</span></label><input id='proxy_host' name='proxy_host' maxlength='63' placeholder='192.168.1.83'></div>"
"<div><label for='proxy_port'>Proxy Port</label><input id='proxy_port' name='proxy_port' type='number' min='1' max='65535' placeholder='7897'></div>"
"</div></details>"
"</section>"
"<div class='actions'><button class='primary' type='submit' id='submit_btn'>Save & Reboot</button><button class='ghost' type='button' onclick='location.reload()'>Reload</button></div>"
"<div class='msg' id='msg'></div>"
"<p class='note'>Sensitive fields are never shown in plain text. Leave them empty to keep current value.</p>"
"</form></div>"
"<script>"
"const stateClass=v=>v?'state on':'state';"
"const stateText=v=>v?'configured':'not set';"
"const setState=(id,v)=>{const e=document.getElementById(id);if(!e)return;e.className=stateClass(v);e.textContent=stateText(v);};"
"const pick=document.getElementById('ssid_pick');const ssid=document.getElementById('ssid');"
"pick.addEventListener('change',()=>{if(pick.value)ssid.value=pick.value;});"
"const msg=document.getElementById('msg');"
"fetch('/api/config-status').then(r=>r.json()).then(s=>{"
" if(s.wifi_ssid)ssid.value=s.wifi_ssid;"
" if(s.llm_model)document.getElementById('model').value=s.llm_model;"
" if(s.llm_base_url)document.getElementById('base_url').value=s.llm_base_url;"
" if(s.proxy_host)document.getElementById('proxy_host').value=s.proxy_host;"
" if(s.proxy_port)document.getElementById('proxy_port').value=s.proxy_port;"
" const pe=document.getElementById('proxy_enabled');if(pe)pe.checked=!!s.proxy_enabled;"
" setState('wifi_state',!!s.wifi_pass_set);"
" setState('tg_state',!!s.tg_token_set);"
" setState('llm_key_state',!!s.llm_api_key_set);"
" setState('search_state',!!s.search_key_set);"
" setState('proxy_state',!!s.proxy_enabled);"
"}).catch(()=>{msg.className='msg err';msg.textContent='Cannot load current status, you can still submit manually.';});"
"document.getElementById('cfg').addEventListener('submit',()=>{"
" const b=document.getElementById('submit_btn');"
" b.disabled=true; b.textContent='Saving...';"
" msg.className='msg ok'; msg.textContent='Saving configuration, device will reboot shortly...';"
"});"
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

static void html_escape(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 1 < dst_size; si++) {
        const char *rep = NULL;
        switch (src[si]) {
            case '&': rep = "&amp;"; break;
            case '<': rep = "&lt;"; break;
            case '>': rep = "&gt;"; break;
            case '"': rep = "&quot;"; break;
            case '\'': rep = "&#39;"; break;
            default: break;
        }
        if (!rep) {
            dst[di++] = src[si];
            continue;
        }
        size_t rep_len = strlen(rep);
        if (di + rep_len >= dst_size) break;
        memcpy(dst + di, rep, rep_len);
        di += rep_len;
    }
    dst[di] = '\0';
}

static bool nvs_get_string_value(const char *ns, const char *key, char *out, size_t out_size)
{
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    size_t len = out_size;
    esp_err_t err = nvs_get_str(nvs, key, out, &len);
    nvs_close(nvs);
    return (err == ESP_OK && out[0] != '\0');
}

static bool nvs_get_u16_value(const char *ns, const char *key, uint16_t *out)
{
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_u16(nvs, key, out);
    nvs_close(nvs);
    return err == ESP_OK;
}

static void load_portal_status(portal_status_t *st)
{
    memset(st, 0, sizeof(*st));

    if (!nvs_get_string_value(MIMI_NVS_WIFI, MIMI_NVS_KEY_SSID, st->wifi_ssid, sizeof(st->wifi_ssid)) &&
        MIMI_SECRET_WIFI_SSID[0] != '\0') {
        strncpy(st->wifi_ssid, MIMI_SECRET_WIFI_SSID, sizeof(st->wifi_ssid) - 1);
    }

    char tmp[256] = {0};
    if (nvs_get_string_value(MIMI_NVS_WIFI, MIMI_NVS_KEY_PASS, tmp, sizeof(tmp))) {
        st->wifi_pass_set = true;
    } else if (MIMI_SECRET_WIFI_PASS[0] != '\0') {
        st->wifi_pass_set = true;
    }

    if (nvs_get_string_value(MIMI_NVS_TG, MIMI_NVS_KEY_TG_TOKEN, tmp, sizeof(tmp))) {
        st->tg_token_set = true;
    } else if (MIMI_SECRET_TG_TOKEN[0] != '\0') {
        st->tg_token_set = true;
    }

    if (nvs_get_string_value(MIMI_NVS_LLM, MIMI_NVS_KEY_API_KEY, tmp, sizeof(tmp))) {
        st->llm_api_key_set = true;
    } else if (MIMI_LLM_API_KEY_DEFAULT[0] != '\0' || MIMI_SECRET_API_KEY[0] != '\0') {
        st->llm_api_key_set = true;
    }

    if (!nvs_get_string_value(MIMI_NVS_LLM, MIMI_NVS_KEY_MODEL, st->llm_model, sizeof(st->llm_model))) {
        if (MIMI_SECRET_MODEL[0] != '\0') {
            strncpy(st->llm_model, MIMI_SECRET_MODEL, sizeof(st->llm_model) - 1);
        } else {
            strncpy(st->llm_model, MIMI_LLM_DEFAULT_MODEL, sizeof(st->llm_model) - 1);
        }
    }

    if (!nvs_get_string_value(MIMI_NVS_LLM, MIMI_NVS_KEY_BASE_URL, st->llm_base_url, sizeof(st->llm_base_url))) {
        if (MIMI_SECRET_LLM_BASE_URL[0] != '\0') {
            strncpy(st->llm_base_url, MIMI_SECRET_LLM_BASE_URL, sizeof(st->llm_base_url) - 1);
        } else {
            strncpy(st->llm_base_url, MIMI_LLM_API_URL, sizeof(st->llm_base_url) - 1);
        }
    }

    if (nvs_get_string_value(MIMI_NVS_SEARCH, MIMI_NVS_KEY_API_KEY, tmp, sizeof(tmp))) {
        st->search_key_set = true;
    } else if (MIMI_SECRET_SEARCH_KEY[0] != '\0') {
        st->search_key_set = true;
    }

    bool host_from_nvs = nvs_get_string_value(MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_HOST,
                                              st->proxy_host, sizeof(st->proxy_host));
    bool port_from_nvs = nvs_get_u16_value(MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_PORT, &st->proxy_port);
    if (host_from_nvs && port_from_nvs && st->proxy_port > 0) {
        st->proxy_enabled = true;
    } else if (MIMI_SECRET_PROXY_HOST[0] != '\0' && MIMI_SECRET_PROXY_PORT[0] != '\0') {
        strncpy(st->proxy_host, MIMI_SECRET_PROXY_HOST, sizeof(st->proxy_host) - 1);
        st->proxy_port = (uint16_t)atoi(MIMI_SECRET_PROXY_PORT);
        st->proxy_enabled = (st->proxy_port > 0);
    }
}

static bool parse_form_value(const char *body, const char *key, char *out, size_t out_size)
{
    if (!body || !key || !out || out_size == 0) return false;

    char pattern[64];
    int n = snprintf(pattern, sizeof(pattern), "%s=", key);
    if (n <= 0 || (size_t)n >= sizeof(pattern)) return false;

    const char *p = body;
    size_t key_len = strlen(pattern);
    while (p && *p) {
        if ((p == body || p[-1] == '&') && strncmp(p, pattern, key_len) == 0) {
            const char *start = p + key_len;
            const char *end = strchr(start, '&');
            size_t len = end ? (size_t)(end - start) : strlen(start);

            char *tmp = malloc(len + 1);
            if (!tmp) return false;
            memcpy(tmp, start, len);
            tmp[len] = '\0';
            url_decode(out, out_size, tmp);
            free(tmp);
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }

    return false;
}

static esp_err_t recv_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (!req || !buf || buf_size == 0) return ESP_ERR_INVALID_ARG;
    if (req->content_len <= 0 || (size_t)req->content_len >= buf_size) return ESP_ERR_INVALID_SIZE;

    int total = 0;
    while (total < req->content_len) {
        int r = httpd_req_recv(req, buf + total, req->content_len - total);
        if (r <= 0) {
            return ESP_FAIL;
        }
        total += r;
    }
    buf[total] = '\0';
    return ESP_OK;
}

static bool parse_u16(const char *s, uint16_t *out)
{
    if (!s || !s[0]) return false;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0' || v < 1 || v > 65535) return false;
    *out = (uint16_t)v;
    return true;
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

        char esc[192];
        html_escape(ssid, esc, sizeof(esc));

        off += snprintf(out + off, out_size - off,
                        "<option value='%s'>%s (%d dBm)</option>",
                        esc, esc, (int)aps[i].rssi);
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
    char *options = calloc(1, 4096);
    if (!options) return httpd_resp_send_500(req);
    build_ssid_options(options, 4096);

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

static esp_err_t handle_status(httpd_req_t *req)
{
    portal_status_t st;
    load_portal_status(&st);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_500(req);
    }

    cJSON_AddStringToObject(root, "wifi_ssid", st.wifi_ssid);
    cJSON_AddBoolToObject(root, "wifi_pass_set", st.wifi_pass_set);
    cJSON_AddBoolToObject(root, "tg_token_set", st.tg_token_set);
    cJSON_AddBoolToObject(root, "llm_api_key_set", st.llm_api_key_set);
    cJSON_AddStringToObject(root, "llm_model", st.llm_model);
    cJSON_AddStringToObject(root, "llm_base_url", st.llm_base_url);
    cJSON_AddBoolToObject(root, "search_key_set", st.search_key_set);
    cJSON_AddBoolToObject(root, "proxy_enabled", st.proxy_enabled);
    cJSON_AddStringToObject(root, "proxy_host", st.proxy_host);
    cJSON_AddNumberToObject(root, "proxy_port", st.proxy_port);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return ret;
}

static esp_err_t handle_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handle_no_content(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handle_redirect_root(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handle_save(httpd_req_t *req)
{
    char body[3073];
    if (recv_body(req, body, sizeof(body)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid payload");
    }

    char ssid[33] = {0};
    char pass[65] = {0};
    char tg_token[128] = {0};
    char llm_api_key[128] = {0};
    char model[64] = {0};
    char base_url[256] = {0};
    char search_key[128] = {0};
    char proxy_host[64] = {0};
    char proxy_port_raw[8] = {0};
    char proxy_enabled_raw[8] = {0};

    parse_form_value(body, "ssid", ssid, sizeof(ssid));
    parse_form_value(body, "password", pass, sizeof(pass));
    parse_form_value(body, "tg_token", tg_token, sizeof(tg_token));
    parse_form_value(body, "llm_api_key", llm_api_key, sizeof(llm_api_key));
    parse_form_value(body, "model", model, sizeof(model));
    parse_form_value(body, "base_url", base_url, sizeof(base_url));
    parse_form_value(body, "search_key", search_key, sizeof(search_key));
    parse_form_value(body, "proxy_host", proxy_host, sizeof(proxy_host));
    parse_form_value(body, "proxy_port", proxy_port_raw, sizeof(proxy_port_raw));
    parse_form_value(body, "proxy_enabled", proxy_enabled_raw, sizeof(proxy_enabled_raw));

    if (!ssid[0]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID is required");
    }

    bool proxy_enabled = (strcmp(proxy_enabled_raw, "1") == 0);
    uint16_t proxy_port = 0;

    if (proxy_enabled) {
        if (!proxy_host[0] || !parse_u16(proxy_port_raw, &proxy_port)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "Proxy enabled requires valid host and port");
        }
    }

    if (base_url[0]) {
        esp_err_t url_err = llm_set_base_url(base_url);
        if (url_err != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "Invalid LLM base URL (must be full HTTPS URL)");
        }
    }

    ESP_LOGI(TAG, "Provision save: ssid=%s pass_len=%d model=%s proxy=%s",
             ssid, (int)strlen(pass), model[0] ? model : "(keep)",
             proxy_enabled ? "on" : "off");

    if (wifi_manager_set_credentials(ssid, pass) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save WiFi");
    }

    if (tg_token[0] && telegram_set_token(tg_token) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save Telegram token");
    }

    if (llm_api_key[0] && llm_set_api_key(llm_api_key) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save LLM API key");
    }

    if (model[0] && llm_set_model(model) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save model");
    }

    if (search_key[0] && tool_web_search_set_key(search_key) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save search key");
    }

    if (proxy_enabled) {
        if (http_proxy_set(proxy_host, proxy_port) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save proxy");
        }
    } else {
        if (http_proxy_clear() != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to clear proxy");
        }
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<html><body style='font-family:sans-serif;padding:24px'>"
        "<h2>Configuration saved</h2>"
        "<p>MimiClaw will reboot in 2 seconds to apply settings.</p>"
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
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 10240;

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
    httpd_uri_t status = {
        .uri = "/api/config-status",
        .method = HTTP_GET,
        .handler = handle_status,
    };
    httpd_uri_t favicon = {
        .uri = "/favicon.ico",
        .method = HTTP_GET,
        .handler = handle_favicon,
    };
    httpd_uri_t undefined_get = {
        .uri = "/undefined",
        .method = HTTP_GET,
        .handler = handle_redirect_root,
    };
    httpd_uri_t undefined_post = {
        .uri = "/undefined",
        .method = HTTP_POST,
        .handler = handle_redirect_root,
    };
    httpd_uri_t root_put = {
        .uri = "/",
        .method = HTTP_PUT,
        .handler = handle_no_content,
    };
    httpd_uri_t save_put = {
        .uri = "/save",
        .method = HTTP_PUT,
        .handler = handle_no_content,
    };

    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &status);
    httpd_register_uri_handler(s_httpd, &favicon);
    httpd_register_uri_handler(s_httpd, &undefined_get);
    httpd_register_uri_handler(s_httpd, &undefined_post);
    httpd_register_uri_handler(s_httpd, &root_put);
    httpd_register_uri_handler(s_httpd, &save_put);

    ESP_LOGI(TAG, "AP provisioning started: SSID=%s, open http://192.168.4.1", MIMI_AP_PROV_SSID);
    return ESP_OK;
}
