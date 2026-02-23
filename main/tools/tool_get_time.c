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
#include "esp_crt_bundle.h"

static const char *TAG = "tool_time";

static const char *MONTHS[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

/* Buffer to catch the incoming header */
static char s_date_header[64] = {0};

/* Event handler to actively listen for the "Date" header in the response */
static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(evt->header_key, "Date") == 0) {
            strncpy(s_date_header, evt->header_value, sizeof(s_date_header) - 1);
        }
    }
    return ESP_OK;
}

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
        if (strcmp(mon_str, MONTHS[i]) == 0) {
            mon = i;
            break;
        }
    }
    if (mon == -1) return false;

    struct tm tm_time = {0};
    tm_time.tm_year = year - 1900;
    tm_time.tm_mon = mon;
    tm_time.tm_mday = day;
    tm_time.tm_hour = hour;
    tm_time.tm_min = min;
    tm_time.tm_sec = sec;

    /* Temporarily set TZ to GMT to convert the parsed string correctly */
    setenv("TZ", "GMT0", 1);
    tzset();
    time_t t = mktime(&tm_time);

    /* Set the ESP32 hardware system time */
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    /* Set the timezone permanently for Sabah (UTC+8) */
    setenv("TZ", "MYT-8", 1);
    tzset();

    /* Format the output string in local time for the LLM to read */
    struct tm local_tm;
    localtime_r(&t, &local_tm);
    strftime(out, out_size, "%Y-%m-%d %I:%M %p", &local_tm);

    ESP_LOGI(TAG, "System time synced! Local Time: %s", out);
    return true;
}

static esp_err_t fetch_time_direct(char *out, size_t out_size)
{
    /* Clear the buffer before making the request */
    s_date_header[0] = '\0';

    esp_http_client_config_t config = {
        .url = "https://clients3.google.com/generate_204",
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .event_handler = _http_event_handler, /* Attach our listener */
        .crt_bundle_attach = esp_crt_bundle_attach, /* Secure TLS bundle */
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_err_t err = esp_http_client_perform(client);
    
    /* Clean up the client memory safely and EXACTLY ONCE */
    esp_http_client_cleanup(client);

    /* Check if the HTTP request failed entirely */
    if (err != ESP_OK) {
        return err;
    }

    /* Check if the event handler successfully caught the Date header */
    if (s_date_header[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    /* Parse and set the time using the caught string */
    bool success = parse_and_set_time(s_date_header, out, out_size);
    return success ? ESP_OK : ESP_FAIL;
}

esp_err_t tool_get_time_execute(const char *input_json, char *output, size_t output_size)
{
    ESP_LOGI(TAG, "Fetching current time...");
    
    esp_err_t err;
    if (http_proxy_is_enabled()) {
        err = fetch_time_direct(output, output_size); 
    } else {
        err = fetch_time_direct(output, output_size);
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error: failed to fetch time (%s)", esp_err_to_name(err));
        snprintf(output, output_size, "Error: failed to fetch time");
        return err;
    }
    
    return ESP_OK;
}