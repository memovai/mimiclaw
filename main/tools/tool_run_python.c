#include "tool_run_python.h"
#include "mimi_config.h"
#include "espnow/espnow_manager.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_python";

esp_err_t tool_run_python_execute(const char *input_json, char *output, size_t output_size)
{
    if (!espnow_is_ready()) {
        snprintf(output, output_size,
                 "Error: ESP-NOW not initialized. Configure peer with 'set espnow_peer XX:XX:XX:XX:XX:XX'");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsString(code) || code->valuestring[0] == '\0') {
        snprintf(output, output_size, "Error: 'code' parameter required");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int timeout = MIMI_ESPNOW_TIMEOUT_MS;
    cJSON *tm = cJSON_GetObjectItem(root, "timeout_ms");
    if (cJSON_IsNumber(tm) && tm->valueint > 0) {
        timeout = tm->valueint;
    }

    ESP_LOGI(TAG, "Sending Python script (%u bytes) to Board B",
             (unsigned)strlen(code->valuestring));

    esp_err_t err = espnow_send_script(code->valuestring, output, output_size, timeout);

    cJSON_Delete(root);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Script execution failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Script result: %.80s%s", output,
                 strlen(output) > 80 ? "..." : "");
    }

    return err;
}
