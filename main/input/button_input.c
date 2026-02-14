#include "button_input.h"
#include "mimi_config.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "button";

static button_callback_t s_callback = NULL;
static QueueHandle_t s_gpio_queue = NULL;

/* All buttons are active LOW */
typedef struct {
    gpio_num_t gpio;
    const char *name;
    button_event_t press_event;
    button_event_t release_event;  /* -1 if no release event */
    bool track_release;
} button_def_t;

static const button_def_t s_buttons[] = {
    {MIMI_BTN_WAKE,   "WAKE",   BTN_EVENT_WAKE_PRESS,   BTN_EVENT_WAKE_RELEASE, true},
    {MIMI_BTN_MUTE,   "MUTE",   BTN_EVENT_MUTE_PRESS,   0, false},
    {MIMI_BTN_VOLUME, "VOLUME", BTN_EVENT_VOLUME_PRESS,  0, false},
};
#define NUM_BUTTONS (sizeof(s_buttons) / sizeof(s_buttons[0]))

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t gpio_num = (uint32_t)arg;
    xQueueSendFromISR(s_gpio_queue, &gpio_num, NULL);
}

static void button_task(void *arg)
{
    uint32_t gpio_num;
    TickType_t last_event[NUM_BUTTONS] = {0};
    const TickType_t debounce_ticks = pdMS_TO_TICKS(50);

    while (1) {
        if (xQueueReceive(s_gpio_queue, &gpio_num, portMAX_DELAY)) {
            /* Find which button */
            for (int i = 0; i < NUM_BUTTONS; i++) {
                if (s_buttons[i].gpio == (gpio_num_t)gpio_num) {
                    TickType_t now = xTaskGetTickCount();
                    if ((now - last_event[i]) < debounce_ticks) break;  /* debounce */
                    last_event[i] = now;

                    int level = gpio_get_level(s_buttons[i].gpio);
                    if (level == 0) {
                        /* Pressed (active LOW) */
                        ESP_LOGI(TAG, "%s pressed", s_buttons[i].name);
                        if (s_callback) s_callback(s_buttons[i].press_event);
                    } else if (s_buttons[i].track_release) {
                        /* Released */
                        ESP_LOGI(TAG, "%s released", s_buttons[i].name);
                        if (s_callback) s_callback(s_buttons[i].release_event);
                    }
                    break;
                }
            }
        }
    }
}

esp_err_t button_input_init(button_callback_t cb)
{
    s_callback = cb;
    s_gpio_queue = xQueueCreate(10, sizeof(uint32_t));
    if (!s_gpio_queue) return ESP_ERR_NO_MEM;

    /* Configure all button GPIOs */
    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << s_buttons[i].gpio),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        esp_err_t ret = gpio_config(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "GPIO %d config failed: %s", s_buttons[i].gpio, esp_err_to_name(ret));
            continue;
        }
        ESP_LOGI(TAG, "%s button configured (GPIO %d, level=%d)",
                 s_buttons[i].name, s_buttons[i].gpio, gpio_get_level(s_buttons[i].gpio));
    }

    /* Install ISR service and attach handlers */
    gpio_install_isr_service(0);
    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_isr_handler_add(s_buttons[i].gpio, gpio_isr_handler, (void *)(uint32_t)s_buttons[i].gpio);
    }

    /* Debounce task */
    xTaskCreate(button_task, "buttons", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Button input initialized (3 buttons)");
    return ESP_OK;
}
