#include "ui/config_screen.h"

#include "esp_log.h"

static const char *TAG = "config_screen";
static bool s_active = false;

void config_screen_init(void)
{
    s_active = false;
}

void config_screen_toggle(void)
{
    s_active = !s_active;
    ESP_LOGI(TAG, "Config screen is disabled (active=%s)", s_active ? "true" : "false");
}

bool config_screen_is_active(void)
{
    return s_active;
}

void config_screen_scroll_down(void)
{
    if (s_active) {
        ESP_LOGI(TAG, "Config screen scrolling is disabled");
    }
}
