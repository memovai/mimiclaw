#include "tool_get_time.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_http_client.h"

static const char *TAG = "tool_time";

static const char *MONTHS[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

typedef struct {
    char date_value[64];
} time_http_ctx_t;

/* Parse "Sat, 01 Feb 2025 10:25:00 GMT" → set system clock, return formatted string */
static bool parse_and_set_time(const char *date_str, char *out, size_t out_size)
{
    int day, year, hour, min, sec;
    char mon_str[4] = {0};

    if (sscanf(date_str, "%*[^,], %d %3s %d %d:%d:%d",
               &day, mon_str, &year, &hour, &min, &sec) != 6) {
        return false;
    }

    int mon = -1;
    for (int i = 0; i < 12; i++) {
        if (strcmp(mon_str, MONTHS[i]) == 0) { mon = i; break; }
    }
    if (mon < 0) return false;

    struct tm tm = {
        .tm_sec = sec, .tm_min = min, .tm_hour = hour,
        .tm_mday = day, .tm_mon = mon, .tm_year = year - 1900,
    };

    /* Convert UTC to epoch — mktime expects local, so temporarily set UTC */
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t t = mktime(&tm);

    /* Restore timezone */
    setenv("TZ", MIMI_TIMEZONE, 1);
    tzset();

    if (t < 0) return false;

    struct timeval tv = { .tv_sec = t };
    settimeofday(&tv, NULL);

    /* Format in local time */
    struct tm local;
    localtime_r(&t, &local);
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S %Z (%A)", &local);

    return true;
}

/* Fetch time via proxy: HEAD request to api.telegram.org, parse Date header */
static esp_err_t fetch_time_via_proxy(char *out, size_t out_size)
{
    proxy_conn_t *conn = proxy_conn_open("api.telegram.org", 443, 10000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    const char *req =
        "HEAD / HTTP/1.1\r\n"
        "Host: api.telegram.org\r\n"
        "Connection: close\r\n\r\n";

    if (proxy_conn_write(conn, req, strlen(req)) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    char buf[1024];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int n = proxy_conn_read(conn, buf + total, sizeof(buf) - 1 - total, 10000);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    proxy_conn_close(conn);

    /* Find Date header */
    char *date_hdr = strcasestr(buf, "\r\nDate: ");
    if (!date_hdr) return ESP_ERR_NOT_FOUND;
    date_hdr += 8;

    char *eol = strstr(date_hdr, "\r\n");
    if (!eol) return ESP_ERR_NOT_FOUND;

    char date_val[64];
    size_t dlen = eol - date_hdr;
    if (dlen >= sizeof(date_val)) return ESP_ERR_NOT_FOUND;
    memcpy(date_val, date_hdr, dlen);
    date_val[dlen] = '\0';

    if (!parse_and_set_time(date_val, out, out_size)) return ESP_FAIL;
    return ESP_OK;
}

/* Capture Date header in a stable local buffer. */
static esp_err_t date_header_capture_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER || !evt->user_data) {
        return ESP_OK;
    }

    if (!evt->header_key || !evt->header_value || strcasecmp(evt->header_key, "Date") != 0) {
        return ESP_OK;
    }

    time_http_ctx_t *ctx = (time_http_ctx_t *)evt->user_data;
    size_t len = strlen(evt->header_value);
    if (len >= sizeof(ctx->date_value)) {
        len = sizeof(ctx->date_value) - 1;
    }
    memcpy(ctx->date_value, evt->header_value, len);
    ctx->date_value[len] = '\0';

    return ESP_OK;
}

/* Fetch time via direct HTTP */
static esp_err_t fetch_time_direct(char *out, size_t out_size)
{
    time_http_ctx_t ctx = {0};
    esp_http_client_config_t config = {
        .url = "http://captive.apple.com/",
        .method = HTTP_METHOD_HEAD,
        .timeout_ms = 10000,
        .event_handler = date_header_capture_handler,
        .user_data = &ctx,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_cleanup(client);

    if (ctx.date_value[0] == '\0') return ESP_ERR_NOT_FOUND;

    if (!parse_and_set_time(ctx.date_value, out, out_size)) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t tool_get_time_execute(const char *input_json, char *output, size_t output_size)
{
    ESP_LOGI(TAG, "Fetching current time...");

    esp_err_t err;
    if (http_proxy_is_enabled()) {
        err = fetch_time_via_proxy(output, output_size);
    } else {
        err = fetch_time_direct(output, output_size);
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Time: %s", output);
    } else {
        snprintf(output, output_size, "Error: failed to fetch time (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", output);
    }

    return err;
}
