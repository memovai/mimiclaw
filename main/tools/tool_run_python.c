#include "tool_run_python.h"
#include "micropython/micropython_vm.h"
#include "mimi_config.h"

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_python";

esp_err_t tool_run_python_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *code_obj = cJSON_GetObjectItem(root, "code");
    if (!code_obj || !cJSON_IsString(code_obj)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: 'code' field is required and must be a string");
        return ESP_ERR_INVALID_ARG;
    }

    /* Optional timeout (default from config, clamped 100ms–30s) */
    int timeout_ms = MIMI_MICROPYTHON_TIMEOUT_MS;
    cJSON *timeout_obj = cJSON_GetObjectItem(root, "timeout_ms");
    if (timeout_obj && cJSON_IsNumber(timeout_obj)) {
        timeout_ms = timeout_obj->valueint;
        if (timeout_ms < 100)   timeout_ms = 100;
        if (timeout_ms > 30000) timeout_ms = 30000;
    }

    ESP_LOGI(TAG, "Running Python (%d bytes code, timeout=%dms)",
             (int)strlen(code_obj->valuestring), timeout_ms);

    esp_err_t err = micropython_vm_exec(code_obj->valuestring,
                                         output, output_size, timeout_ms);

    cJSON_Delete(root);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Execution failed: %s", esp_err_to_name(err));
    }
    return err;
}
