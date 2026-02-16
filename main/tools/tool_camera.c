#include "tools/tool_camera.h"
#include "esp_camera.h"
#include "esp_log.h"           // [新增] 讓你使用 ESP_LOGI
#include "freertos/FreeRTOS.h" // [新增] 定義 pdMS_TO_TICKS
#include "freertos/task.h"

static camera_fb_t *s_last_fb = NULL;

camera_fb_t* tool_camera_get_last_fb(void) {
    return s_last_fb;
}

void tool_camera_clear_last_fb(void) {
    if (s_last_fb) {
        esp_camera_fb_return(s_last_fb);
        s_last_fb = NULL;
    }
}

esp_err_t tool_take_photo_execute(const char *input_json, char *output, size_t output_size) {
    tool_camera_clear_last_fb();

    // camera warm up
    for (int i = 0; i < 2; i++) {
        camera_fb_t *dummy = esp_camera_fb_get();
        if (dummy) {
            esp_camera_fb_return(dummy);
        }
        vTaskDelay(pdMS_TO_TICKS(40)); 
    }

    s_last_fb = esp_camera_fb_get();
    if (!s_last_fb) {
        snprintf(output, output_size, "Error: Camera hardware failed to capture.");
        return ESP_FAIL;
    }

    snprintf(output, output_size, "__MIMI_PHOTO_READY__");
    return ESP_OK;
}