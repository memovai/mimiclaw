#include "ui/config_screen.h"

#include <stdio.h>
#include <string.h>

#include "display/display.h"
#include "display/font5x7.h"
#include "wifi/wifi_manager.h"
#include "mimi_config.h"
#include "mimi_secrets.h"
#include "nvs.h"
#include "esp_log.h"

#define CONFIG_LINE_MAX 64
#define CONFIG_LINES_MAX 12

static const char *TAG = "config_screen";

static char s_lines[CONFIG_LINES_MAX][CONFIG_LINE_MAX];
static const char *s_line_ptrs[CONFIG_LINES_MAX];
static size_t s_line_count = 0;
static size_t s_scroll = 0;
static bool s_active = false;
static size_t s_selected = 0;
static int s_sel_offset_px = 0;

#define QR_BOX 110
#define LEFT_PAD 6
#define RIGHT_X (LEFT_PAD + QR_BOX + 10)
#define RIGHT_W (DISPLAY_WIDTH - RIGHT_X - 6)
#define FONT_SCALE 2
#define CHAR_W ((FONT5X7_WIDTH + 1) * FONT_SCALE)

static void build_line(char *out, size_t out_len, const char *label,
                       const char *ns, const char *key,
                       const char *build_val, bool mask)
{
    char nvs_val[128] = {0};
    const char *source = "not set";
    const char *display = "(empty)";

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(nvs_val);
        if (nvs_get_str(nvs, key, nvs_val, &len) == ESP_OK && nvs_val[0]) {
            source = "NVS";
            display = nvs_val;
        }
        nvs_close(nvs);
    }

    if (strcmp(source, "not set") == 0 && build_val[0] != '\0') {
        source = "build";
        display = build_val;
    }

    char masked[32] = {0};
    if (mask && strcmp(display, "(empty)") != 0) {
        size_t dlen = strlen(display);
        if (dlen > 4) {
            snprintf(masked, sizeof(masked), "%.4s****", display);
            display = masked;
        }
    }

    snprintf(out, out_len, "%s: %s [%s]", label, display, source);
}

static void build_config_lines(void)
{
    s_line_count = 0;

    build_line(s_lines[s_line_count++], CONFIG_LINE_MAX, "WiFi SSID",  MIMI_NVS_WIFI,   MIMI_NVS_KEY_SSID,     MIMI_SECRET_WIFI_SSID,  false);
    build_line(s_lines[s_line_count++], CONFIG_LINE_MAX, "WiFi Pass",  MIMI_NVS_WIFI,   MIMI_NVS_KEY_PASS,     MIMI_SECRET_WIFI_PASS,  true);
    build_line(s_lines[s_line_count++], CONFIG_LINE_MAX, "TG Token",   MIMI_NVS_TG,     MIMI_NVS_KEY_TG_TOKEN, MIMI_SECRET_TG_TOKEN,   true);
    build_line(s_lines[s_line_count++], CONFIG_LINE_MAX, "API Key",    MIMI_NVS_LLM,    MIMI_NVS_KEY_API_KEY,  MIMI_SECRET_API_KEY,    true);
    build_line(s_lines[s_line_count++], CONFIG_LINE_MAX, "Model",      MIMI_NVS_LLM,    MIMI_NVS_KEY_MODEL,    MIMI_SECRET_MODEL,      false);
    build_line(s_lines[s_line_count++], CONFIG_LINE_MAX, "Provider",   MIMI_NVS_LLM,    MIMI_NVS_KEY_PROVIDER, MIMI_SECRET_MODEL_PROVIDER, false);
    build_line(s_lines[s_line_count++], CONFIG_LINE_MAX, "Proxy Host", MIMI_NVS_PROXY,  MIMI_NVS_KEY_PROXY_HOST, MIMI_SECRET_PROXY_HOST, false);
    build_line(s_lines[s_line_count++], CONFIG_LINE_MAX, "Proxy Port", MIMI_NVS_PROXY,  MIMI_NVS_KEY_PROXY_PORT, MIMI_SECRET_PROXY_PORT, false);
    build_line(s_lines[s_line_count++], CONFIG_LINE_MAX, "Search Key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_API_KEY,  MIMI_SECRET_SEARCH_KEY, true);

    for (size_t i = 0; i < s_line_count; i++) {
        s_line_ptrs[i] = s_lines[i];
    }
}

static void render_config_screen(void)
{
    const char *ip = wifi_manager_get_ip();
    if (!ip || ip[0] == '\0') {
        ip = "0.0.0.0";
    }

    char qr_text[64] = {0};
    char ip_text[64] = {0};
    snprintf(qr_text, sizeof(qr_text), "%s:8888", ip);
    snprintf(ip_text, sizeof(ip_text), "%s:8888", ip);

    display_show_config_screen(qr_text, ip_text, s_line_ptrs, s_line_count, s_scroll, s_selected, s_sel_offset_px);
}

void config_screen_init(void)
{
    build_config_lines();
}

void config_screen_toggle(void)
{
    if (s_active) {
        s_active = false;
        display_show_banner();
        return;
    }

    build_config_lines();
    s_scroll = 0;
    s_selected = 0;
    s_sel_offset_px = 0;
    s_active = true;
    ESP_LOGI(TAG, "Switch to config screen");
    render_config_screen();
}

bool config_screen_is_active(void)
{
    return s_active;
}

void config_screen_scroll_down(void)
{
    if (!s_active || s_line_count == 0) {
        return;
    }

    s_scroll++;
    if (s_scroll >= s_line_count) {
        s_scroll = 0;
    }
    s_selected = s_scroll;
    s_sel_offset_px = 0;
    render_config_screen();
}
