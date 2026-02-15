#include "tool_get_time.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "tool_time";

static const char *MONTHS[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

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
    proxy_conn_t *conn = proxy_conn_open("api.telegram.org", 443, MIMI_PROXY_CONNECT_TIMEOUT_MS);
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
        int n = proxy_conn_read(conn, buf + total, sizeof(buf) - 1 - total, MIMI_PROXY_IO_TIMEOUT_MS);
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

/* Fetch time via direct HTTPS */
static esp_err_t fetch_time_direct(char *out, size_t out_size)
{
    esp_http_client_config_t config = {
        .url = "https://api.telegram.org/",
        .method = HTTP_METHOD_HEAD,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    /* Get Date header */
    char date_val[64] = {0};
    err = esp_http_client_get_header(client, "Date", (char **)&date_val);
    /* esp_http_client_get_header returns pointer, not copy */
    char *date_ptr = NULL;
    esp_http_client_get_header(client, "Date", &date_ptr);
    esp_http_client_cleanup(client);

    if (!date_ptr || date_ptr[0] == '\0') return ESP_ERR_NOT_FOUND;

    if (!parse_and_set_time(date_ptr, out, out_size)) return ESP_FAIL;
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
