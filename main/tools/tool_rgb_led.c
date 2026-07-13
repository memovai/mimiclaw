#include "tools/tool_rgb_led.h"
#include "mimi_config.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "led_strip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "tool_rgb_led";
static led_strip_handle_t s_strip;
static int s_r = 0, s_g = 0, s_b = 0, s_brightness = 255;
static int s_last_r = 255, s_last_g = 180, s_last_b = 80;
static TaskHandle_t s_effect_task;
static volatile bool s_effect_stop;
static char s_effect_name[16] = "none";
static int s_effect_r = 255, s_effect_g = 180, s_effect_b = 80;
static int s_effect_r2 = 0, s_effect_g2 = 0, s_effect_b2 = 255;
static int s_effect_brightness = 128;
static int s_effect_speed_ms = 500;

static int clamp_u8(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return value;
}

static int scale_u8(int value, int brightness)
{
    return (value * brightness + 127) / 255;
}

static int clamp_range(int value, int min, int max)
{
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static bool is_lit(int r, int g, int b)
{
    return r != 0 || g != 0 || b != 0;
}

static bool color_name_is(const char *name, const char *english, const char *chinese)
{
    char chinese_color[16];
    snprintf(chinese_color, sizeof(chinese_color), "%s色", chinese);
    return strcasecmp(name, english) == 0 ||
           strcmp(name, chinese) == 0 ||
           strcmp(name, chinese_color) == 0;
}

static bool color_name_matches(const char *name, const char *a, const char *b, const char *c)
{
    return (a && strcasecmp(name, a) == 0) ||
           (b && strcmp(name, b) == 0) ||
           (c && strcmp(name, c) == 0);
}

static bool color_name_is_on(const char *name)
{
    return color_name_matches(name, "on", "开", "开灯") ||
           color_name_matches(name, "light_on", "打开", "打开灯");
}

static bool color_name_to_rgb(const char *name, int *r, int *g, int *b)
{
    if (!name) return false;
    if (color_name_matches(name, "off", "关", "关灯") ||
        color_name_matches(name, "light_off", "关闭", "关闭灯") ||
        color_name_is(name, "black", "黑")) {
        *r = 0; *g = 0; *b = 0; return true;
    }
    if (color_name_is(name, "red", "红")) {
        *r = 255; *g = 0; *b = 0; return true;
    }
    if (color_name_is(name, "green", "绿")) {
        *r = 0; *g = 255; *b = 0; return true;
    }
    if (color_name_is(name, "blue", "蓝")) {
        *r = 0; *g = 0; *b = 255; return true;
    }
    if (color_name_is(name, "white", "白")) {
        *r = 255; *g = 255; *b = 255; return true;
    }
    if (strcasecmp(name, "cool_white") == 0 || strcmp(name, "冷白") == 0) {
        *r = 180; *g = 220; *b = 255; return true;
    }
    if (color_name_is(name, "yellow", "黄")) {
        *r = 255; *g = 255; *b = 0; return true;
    }
    if (color_name_is(name, "cyan", "青")) {
        *r = 0; *g = 255; *b = 255; return true;
    }
    if (color_name_is(name, "purple", "紫") ||
        color_name_is(name, "violet", "紫罗兰") ||
        strcasecmp(name, "magenta") == 0) {
        *r = 128; *g = 0; *b = 255; return true;
    }
    if (color_name_is(name, "orange", "橙")) {
        *r = 255; *g = 80; *b = 0; return true;
    }
    if (color_name_is(name, "pink", "粉")) {
        *r = 255; *g = 40; *b = 120; return true;
    }
    if (color_name_is(name, "rose", "玫红")) {
        *r = 255; *g = 0; *b = 80; return true;
    }
    if (color_name_is(name, "teal", "蓝绿")) {
        *r = 0; *g = 160; *b = 160; return true;
    }
    if (color_name_is(name, "warm_white", "暖白")) {
        *r = 255; *g = 180; *b = 80; return true;
    }
    return false;
}

static bool parse_hex_color(const char *hex, int *r, int *g, int *b)
{
    if (!hex) return false;
    if (hex[0] == '#') hex++;
    if (strlen(hex) != 6) return false;

    char *end = NULL;
    long value = strtol(hex, &end, 16);
    if (!end || *end != '\0' || value < 0 || value > 0xFFFFFF) return false;

    *r = (value >> 16) & 0xFF;
    *g = (value >> 8) & 0xFF;
    *b = value & 0xFF;
    return true;
}

static void wheel_color(int pos, int *r, int *g, int *b)
{
    pos = 255 - clamp_u8(pos);
    if (pos < 85) {
        *r = 255 - pos * 3; *g = 0; *b = pos * 3; return;
    }
    if (pos < 170) {
        pos -= 85;
        *r = 0; *g = pos * 3; *b = 255 - pos * 3; return;
    }
    pos -= 170;
    *r = pos * 3; *g = 255 - pos * 3; *b = 0;
}

static void apply_brightness_json(cJSON *root, int *brightness)
{
    cJSON *brightness_obj = cJSON_GetObjectItem(root, "brightness");
    if (cJSON_IsNumber(brightness_obj)) {
        *brightness = clamp_u8(brightness_obj->valueint);
    }

    cJSON *percent_obj = cJSON_GetObjectItem(root, "brightness_percent");
    if (cJSON_IsNumber(percent_obj)) {
        int pct = percent_obj->valueint;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        *brightness = (pct * 255 + 50) / 100;
    }

    cJSON *delta_obj = cJSON_GetObjectItem(root, "delta_brightness");
    if (cJSON_IsNumber(delta_obj)) {
        *brightness = clamp_u8(*brightness + delta_obj->valueint);
    }
}

static bool apply_color_json(cJSON *root, int *r, int *g, int *b)
{
    cJSON *color = cJSON_GetObjectItem(root, "color");
    if (cJSON_IsString(color)) {
        if (color_name_is_on(color->valuestring)) {
            *r = s_last_r; *g = s_last_g; *b = s_last_b;
            return true;
        }
        return color_name_to_rgb(color->valuestring, r, g, b);
    }

    cJSON *hex = cJSON_GetObjectItem(root, "hex");
    if (cJSON_IsString(hex)) {
        return parse_hex_color(hex->valuestring, r, g, b);
    }

    cJSON *r_obj = cJSON_GetObjectItem(root, "r");
    cJSON *g_obj = cJSON_GetObjectItem(root, "g");
    cJSON *b_obj = cJSON_GetObjectItem(root, "b");
    if (cJSON_IsNumber(r_obj) && cJSON_IsNumber(g_obj) && cJSON_IsNumber(b_obj)) {
        *r = clamp_u8(r_obj->valueint);
        *g = clamp_u8(g_obj->valueint);
        *b = clamp_u8(b_obj->valueint);
        return true;
    }

    return false;
}

static bool apply_color_json_suffix(cJSON *root, const char *suffix, int *r, int *g, int *b)
{
    char key[8];

    snprintf(key, sizeof(key), "color%s", suffix);
    cJSON *color = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(color)) {
        return color_name_to_rgb(color->valuestring, r, g, b);
    }

    snprintf(key, sizeof(key), "hex%s", suffix);
    cJSON *hex = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(hex)) {
        return parse_hex_color(hex->valuestring, r, g, b);
    }

    char rk[4], gk[4], bk[4];
    snprintf(rk, sizeof(rk), "r%s", suffix);
    snprintf(gk, sizeof(gk), "g%s", suffix);
    snprintf(bk, sizeof(bk), "b%s", suffix);
    cJSON *r_obj = cJSON_GetObjectItem(root, rk);
    cJSON *g_obj = cJSON_GetObjectItem(root, gk);
    cJSON *b_obj = cJSON_GetObjectItem(root, bk);
    if (cJSON_IsNumber(r_obj) && cJSON_IsNumber(g_obj) && cJSON_IsNumber(b_obj)) {
        *r = clamp_u8(r_obj->valueint);
        *g = clamp_u8(g_obj->valueint);
        *b = clamp_u8(b_obj->valueint);
        return true;
    }

    return false;
}

static const char *normalize_effect(const char *name)
{
    if (!name) return "stop";
    if (strcasecmp(name, "stop") == 0 || strcmp(name, "停止") == 0 || strcmp(name, "停止灯效") == 0) return "stop";
    if (strcasecmp(name, "blink") == 0 || strcmp(name, "闪烁") == 0 || strcmp(name, "闪") == 0) return "blink";
    if (strcasecmp(name, "alternate") == 0 || strcasecmp(name, "swap") == 0 || strcmp(name, "交替") == 0 || strcmp(name, "相间") == 0) return "alternate";
    if (strcasecmp(name, "breathe") == 0 || strcasecmp(name, "breath") == 0 || strcmp(name, "呼吸") == 0) return "breathe";
    if (strcasecmp(name, "fade") == 0 || strcasecmp(name, "color_fade") == 0 || strcmp(name, "渐变") == 0 || strcmp(name, "淡入淡出") == 0) return "fade";
    if (strcasecmp(name, "rainbow") == 0 || strcmp(name, "彩虹") == 0) return "rainbow";
    if (strcasecmp(name, "pulse") == 0 || strcmp(name, "脉冲") == 0) return "pulse";
    if (strcasecmp(name, "heartbeat") == 0 || strcmp(name, "心跳") == 0) return "heartbeat";
    if (strcasecmp(name, "sparkle") == 0 || strcasecmp(name, "strobe") == 0 || strcmp(name, "闪光") == 0 || strcmp(name, "频闪") == 0) return "sparkle";
    if (strcasecmp(name, "confetti") == 0 || strcmp(name, "彩纸") == 0 || strcmp(name, "彩色纸屑") == 0 || strcmp(name, "纸屑") == 0 || strcmp(name, "彩点") == 0 || strcmp(name, "随机彩点") == 0) return "confetti";
    if (strcasecmp(name, "police") == 0 || strcmp(name, "警灯") == 0 || strcmp(name, "红蓝") == 0) return "police";
    return NULL;
}

static bool signal_is(const char *signal, const char *english, const char *chinese)
{
    return strcasecmp(signal, english) == 0 || strcmp(signal, chinese) == 0;
}

static const char *signal_json(const char *signal, int pct, char *buf, size_t size)
{
    if (signal_is(signal, "idle", "空闲") || signal_is(signal, "ready", "就绪") || signal_is(signal, "online", "在线") || strcmp(signal, "默认") == 0) {
        snprintf(buf, size, "{\"effect\":\"breathe\",\"color\":\"warm_white\",\"brightness_percent\":%d,\"speed_ms\":120}", pct >= 0 ? pct : 5);
    } else if (signal_is(signal, "thinking", "思考") || signal_is(signal, "processing", "处理中")) {
        snprintf(buf, size, "{\"effect\":\"breathe\",\"color\":\"cool_white\",\"brightness_percent\":%d,\"speed_ms\":70}", pct >= 0 ? pct : 15);
    } else if (signal_is(signal, "tool", "工具") || signal_is(signal, "working", "工作中") || strcmp(signal, "工具执行") == 0) {
        snprintf(buf, size, "{\"effect\":\"pulse\",\"color\":\"cyan\",\"brightness_percent\":%d,\"speed_ms\":90}", pct >= 0 ? pct : 20);
    } else if (signal_is(signal, "success", "成功") || signal_is(signal, "done", "完成") || strcasecmp(signal, "ok") == 0) {
        snprintf(buf, size, "{\"effect\":\"pulse\",\"color\":\"green\",\"brightness_percent\":%d,\"speed_ms\":160}", pct >= 0 ? pct : 20);
    } else if (signal_is(signal, "warning", "警告") || signal_is(signal, "warn", "注意")) {
        snprintf(buf, size, "{\"effect\":\"blink\",\"color\":\"yellow\",\"brightness_percent\":%d,\"speed_ms\":500}", pct >= 0 ? pct : 20);
    } else if (signal_is(signal, "error", "错误") || signal_is(signal, "failed", "失败") || strcasecmp(signal, "fail") == 0) {
        snprintf(buf, size, "{\"effect\":\"blink\",\"color\":\"red\",\"brightness_percent\":%d,\"speed_ms\":160}", pct >= 0 ? pct : 30);
    } else if (signal_is(signal, "urgent", "紧急")) {
        snprintf(buf, size, "{\"effect\":\"heartbeat\",\"color\":\"red\",\"brightness_percent\":%d,\"speed_ms\":110}", pct >= 0 ? pct : 40);
    } else if (signal_is(signal, "message", "消息") || signal_is(signal, "notification", "通知")) {
        snprintf(buf, size, "{\"effect\":\"pulse\",\"color\":\"blue\",\"brightness_percent\":%d,\"speed_ms\":180}", pct >= 0 ? pct : 20);
    } else if (signal_is(signal, "important", "重要")) {
        snprintf(buf, size, "{\"effect\":\"breathe\",\"color\":\"purple\",\"brightness_percent\":%d,\"speed_ms\":90}", pct >= 0 ? pct : 25);
    } else if (signal_is(signal, "offline", "离线") || strcmp(signal, "断网") == 0 || strcmp(signal, "网络异常") == 0) {
        snprintf(buf, size, "{\"effect\":\"blink\",\"color\":\"yellow\",\"brightness_percent\":%d,\"speed_ms\":250}", pct >= 0 ? pct : 25);
    } else if (signal_is(signal, "telegram_offline", "telegram异常") || strcmp(signal, "telegram断开") == 0) {
        snprintf(buf, size, "{\"effect\":\"blink\",\"color\":\"blue\",\"brightness_percent\":%d,\"speed_ms\":250}", pct >= 0 ? pct : 25);
    } else if (signal_is(signal, "find_me", "找我") || strcmp(signal, "你在哪") == 0 || strcmp(signal, "定位") == 0) {
        snprintf(buf, size, "{\"effect\":\"rainbow\",\"brightness_percent\":%d,\"speed_ms\":60}", pct >= 0 ? pct : 35);
    } else if (signal_is(signal, "sleep", "睡觉") || signal_is(signal, "night", "夜间")) {
        snprintf(buf, size, "{\"color\":\"warm_white\",\"brightness_percent\":%d}", pct >= 0 ? pct : 5);
        return "set";
    } else if (signal_is(signal, "off", "关灯") || strcmp(signal, "关闭") == 0) {
        snprintf(buf, size, "{\"color\":\"off\"}");
        return "set";
    } else {
        return NULL;
    }
    return "effect";
}

static esp_err_t set_light_state(int r, int g, int b, int brightness)
{
    int out_r = scale_u8(r, brightness);
    int out_g = scale_u8(g, brightness);
    int out_b = scale_u8(b, brightness);

    esp_err_t err = led_strip_set_pixel(s_strip, 0, out_r, out_g, out_b);
    if (err == ESP_OK) err = led_strip_refresh(s_strip);
    if (err != ESP_OK) {
        return err;
    }

    s_r = r;
    s_g = g;
    s_b = b;
    s_brightness = brightness;
    if (is_lit(r, g, b)) {
        s_last_r = r;
        s_last_g = g;
        s_last_b = b;
    }

    return ESP_OK;
}

static esp_err_t write_light(int r, int g, int b, int brightness, char *output, size_t output_size)
{
    esp_err_t err = set_light_state(r, g, b, brightness);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to set light (%s)", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "灯已设置: r=%d g=%d b=%d brightness=%d",
             r, g, b, brightness);
    ESP_LOGI(TAG, "light: GPIO%d r=%d g=%d b=%d brightness=%d -> %d,%d,%d",
             MIMI_RGB_LED_GPIO, r, g, b, brightness,
             scale_u8(r, brightness), scale_u8(g, brightness), scale_u8(b, brightness));
    return ESP_OK;
}

static void stop_effect(void)
{
    if (!s_effect_task) return;
    s_effect_stop = true;
    for (int i = 0; i < 30 && s_effect_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_effect_task) {
        vTaskDelete(s_effect_task);
        s_effect_task = NULL;
    }
    snprintf(s_effect_name, sizeof(s_effect_name), "none");
}

static void effect_task(void *arg)
{
    (void)arg;
    int step = 0;

    while (!s_effect_stop) {
        if (strcmp(s_effect_name, "blink") == 0) {
            if (step % 2 == 0) {
                set_light_state(s_effect_r, s_effect_g, s_effect_b, s_effect_brightness);
            } else {
                set_light_state(0, 0, 0, s_effect_brightness);
            }
        } else if (strcmp(s_effect_name, "alternate") == 0 || strcmp(s_effect_name, "police") == 0) {
            if (step % 2 == 0) {
                set_light_state(s_effect_r, s_effect_g, s_effect_b, s_effect_brightness);
            } else {
                set_light_state(s_effect_r2, s_effect_g2, s_effect_b2, s_effect_brightness);
            }
        } else if (strcmp(s_effect_name, "breathe") == 0) {
            int phase = step % 64;
            int level = phase < 32 ? phase * 8 : (63 - phase) * 8;
            set_light_state(s_effect_r, s_effect_g, s_effect_b,
                            scale_u8(s_effect_brightness, clamp_u8(level)));
        } else if (strcmp(s_effect_name, "fade") == 0) {
            int phase = step % 64;
            int mix = phase < 32 ? phase * 8 : (63 - phase) * 8;
            int r = (s_effect_r * (255 - mix) + s_effect_r2 * mix) / 255;
            int g = (s_effect_g * (255 - mix) + s_effect_g2 * mix) / 255;
            int b = (s_effect_b * (255 - mix) + s_effect_b2 * mix) / 255;
            set_light_state(r, g, b, s_effect_brightness);
        } else if (strcmp(s_effect_name, "rainbow") == 0) {
            int r, g, b;
            wheel_color((step * 4) & 0xFF, &r, &g, &b);
            set_light_state(r, g, b, s_effect_brightness);
        } else if (strcmp(s_effect_name, "pulse") == 0) {
            int phase = step % 16;
            int level = phase == 0 ? s_effect_brightness :
                        phase < 5 ? scale_u8(s_effect_brightness, 180 - phase * 28) : 0;
            set_light_state(s_effect_r, s_effect_g, s_effect_b, level);
        } else if (strcmp(s_effect_name, "heartbeat") == 0) {
            int phase = step % 12;
            int level = (phase == 0 || phase == 2) ? s_effect_brightness :
                        (phase == 1 || phase == 3) ? scale_u8(s_effect_brightness, 45) : 0;
            set_light_state(s_effect_r, s_effect_g, s_effect_b, level);
        } else if (strcmp(s_effect_name, "sparkle") == 0) {
            int on = (esp_random() % 100) < 35;
            set_light_state(on ? s_effect_r : 0, on ? s_effect_g : 0, on ? s_effect_b : 0, s_effect_brightness);
        } else if (strcmp(s_effect_name, "confetti") == 0) {
            int r, g, b;
            wheel_color(esp_random() & 0xFF, &r, &g, &b);
            set_light_state(r, g, b, s_effect_brightness);
        }
        step++;
        vTaskDelay(pdMS_TO_TICKS(s_effect_speed_ms));
    }

    s_effect_task = NULL;
    s_effect_stop = false;
    vTaskDelete(NULL);
}

esp_err_t tool_rgb_led_init(void)
{
    if (s_strip) return ESP_OK;

    led_strip_config_t strip_config = {
        .strip_gpio_num = MIMI_RGB_LED_GPIO,
        .max_leds = MIMI_RGB_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init RGB LED on GPIO%d: %s", MIMI_RGB_LED_GPIO, esp_err_to_name(err));
        return err;
    }

    led_strip_clear(s_strip);
    strncpy(s_effect_name, "breathe", sizeof(s_effect_name) - 1);
    s_effect_name[sizeof(s_effect_name) - 1] = '\0';
    s_effect_r = 255; s_effect_g = 180; s_effect_b = 80;
    s_effect_r2 = 0; s_effect_g2 = 0; s_effect_b2 = 255;
    s_effect_brightness = 13;
    s_effect_speed_ms = 120;
    s_effect_stop = false;
    if (xTaskCreate(effect_task, "rgb_led_effect", 4096, NULL, 3, &s_effect_task) != pdPASS) {
        snprintf(s_effect_name, sizeof(s_effect_name), "none");
        ESP_LOGW(TAG, "Failed to start default idle light signal");
    }
    ESP_LOGI(TAG, "RGB LED initialized on GPIO%d", MIMI_RGB_LED_GPIO);
    return ESP_OK;
}

esp_err_t tool_rgb_led_set_execute(const char *input_json, char *output, size_t output_size)
{
    esp_err_t err = tool_rgb_led_init();
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: RGB LED init failed on GPIO%d (%s)",
                 MIMI_RGB_LED_GPIO, esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    stop_effect();

    int r = s_r, g = s_g, b = s_b;
    int brightness = s_brightness > 0 ? s_brightness : 255;
    bool have_color = false;

    cJSON *color = cJSON_GetObjectItem(root, "color");
    if (cJSON_IsString(color)) {
        if (color_name_is_on(color->valuestring)) {
            r = s_last_r; g = s_last_g; b = s_last_b;
        } else if (!color_name_to_rgb(color->valuestring, &r, &g, &b)) {
            snprintf(output, output_size, "Error: unknown color '%s'", color->valuestring);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        have_color = true;
    }

    cJSON *hex = cJSON_GetObjectItem(root, "hex");
    if (cJSON_IsString(hex)) {
        if (!parse_hex_color(hex->valuestring, &r, &g, &b)) {
            snprintf(output, output_size, "Error: invalid hex color '%s'", hex->valuestring);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        have_color = true;
    }

    if (!have_color) {
        cJSON *r_obj = cJSON_GetObjectItem(root, "r");
        cJSON *g_obj = cJSON_GetObjectItem(root, "g");
        cJSON *b_obj = cJSON_GetObjectItem(root, "b");
        if (cJSON_IsNumber(r_obj) && cJSON_IsNumber(g_obj) && cJSON_IsNumber(b_obj)) {
            r = clamp_u8(r_obj->valueint);
            g = clamp_u8(g_obj->valueint);
            b = clamp_u8(b_obj->valueint);
            have_color = true;
        }
    }

    apply_brightness_json(root, &brightness);

    if (!have_color && !is_lit(r, g, b)) {
        r = s_last_r;
        g = s_last_g;
        b = s_last_b;
    }

    err = write_light(r, g, b, brightness, output, output_size);
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_rgb_led_effect_execute(const char *input_json, char *output, size_t output_size)
{
    esp_err_t err = tool_rgb_led_init();
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: RGB LED init failed on GPIO%d (%s)",
                 MIMI_RGB_LED_GPIO, esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *effect = cJSON_GetObjectItem(root, "effect");
    const char *name = normalize_effect(cJSON_IsString(effect) ? effect->valuestring : "stop");

    if (!name) {
        snprintf(output, output_size, "Error: unknown effect");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(name, "stop") == 0) {
        stop_effect();
        snprintf(output, output_size, "灯效已停止");
        cJSON_Delete(root);
        return ESP_OK;
    }

    int r = is_lit(s_r, s_g, s_b) ? s_r : s_last_r;
    int g = is_lit(s_r, s_g, s_b) ? s_g : s_last_g;
    int b = is_lit(s_r, s_g, s_b) ? s_b : s_last_b;
    int r2 = 0, g2 = 0, b2 = 255;
    int brightness = s_brightness > 0 ? s_brightness : 128;
    int speed_ms = 500;

    apply_color_json(root, &r, &g, &b);
    apply_color_json_suffix(root, "2", &r2, &g2, &b2);
    if (strcmp(name, "police") == 0) {
        r = 255; g = 0; b = 0;
        r2 = 0; g2 = 0; b2 = 255;
    }
    apply_brightness_json(root, &brightness);

    cJSON *speed = cJSON_GetObjectItem(root, "speed_ms");
    if (cJSON_IsNumber(speed)) {
        speed_ms = clamp_range(speed->valueint, 40, 5000);
    }

    stop_effect();
    strncpy(s_effect_name, name, sizeof(s_effect_name) - 1);
    s_effect_name[sizeof(s_effect_name) - 1] = '\0';
    s_effect_r = r; s_effect_g = g; s_effect_b = b;
    s_effect_r2 = r2; s_effect_g2 = g2; s_effect_b2 = b2;
    s_effect_brightness = brightness;
    s_effect_speed_ms = speed_ms;
    s_effect_stop = false;

    if (xTaskCreate(effect_task, "rgb_led_effect", 4096, NULL, 3, &s_effect_task) != pdPASS) {
        snprintf(s_effect_name, sizeof(s_effect_name), "none");
        snprintf(output, output_size, "Error: failed to start light effect");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    snprintf(output, output_size, "灯效已启动: %s speed_ms=%d brightness=%d", s_effect_name, speed_ms, brightness);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t tool_light_signal_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *signal_obj = cJSON_GetObjectItem(root, "signal");
    if (!cJSON_IsString(signal_obj)) {
        snprintf(output, output_size, "Error: missing signal");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int pct = -1;
    cJSON *pct_obj = cJSON_GetObjectItem(root, "brightness_percent");
    if (cJSON_IsNumber(pct_obj)) {
        pct = clamp_range(pct_obj->valueint, 0, 100);
    }

    char mapped[160];
    const char *target = signal_json(signal_obj->valuestring, pct, mapped, sizeof(mapped));
    if (!target) {
        snprintf(output, output_size, "Error: unknown light signal '%s'", signal_obj->valuestring);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = strcmp(target, "set") == 0 ?
        tool_rgb_led_set_execute(mapped, output, output_size) :
        tool_rgb_led_effect_execute(mapped, output, output_size);
    if (err == ESP_OK) {
        snprintf(output, output_size, "灯信号已设置: %s", signal_obj->valuestring);
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_rgb_led_status_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    snprintf(output, output_size,
             "灯状态: %s, effect=%s, r=%d g=%d b=%d brightness=%d, last=%d,%d,%d",
             is_lit(s_r, s_g, s_b) ? "on" : "off",
             s_effect_task ? s_effect_name : "none",
             s_r, s_g, s_b, s_effect_task ? s_effect_brightness : s_brightness, s_last_r, s_last_g, s_last_b);
    return ESP_OK;
}
