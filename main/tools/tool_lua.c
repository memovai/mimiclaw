#include "tools/tool_lua.h"
#include "tools/lua_mod_gpio.h"
#include "mimi_config.h"
#include "mimi_lua.h"

#include <string.h>
#include <stdio.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "tool_lua";

esp_err_t tool_lua_init(void)
{
    esp_err_t err = lua_mod_gpio_register();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio module registration failed");
        return err;
    }
    ESP_LOGI(TAG, "Lua tools initialized");
    return ESP_OK;
}

/* Extract and validate "path" from the tool input. Returns NULL on error
 * (error text already written to output). Caller owns nothing. */
static const char *get_path(cJSON *root, char *output, size_t output_size)
{
    cJSON *path_obj = cJSON_GetObjectItem(root, "path");
    if (!cJSON_IsString(path_obj) || path_obj->valuestring[0] == '\0') {
        snprintf(output, output_size, "Error: 'path' required (string)");
        return NULL;
    }
    const char *path = path_obj->valuestring;
    if (strncmp(path, MIMI_SPIFFS_BASE "/", strlen(MIMI_SPIFFS_BASE) + 1) != 0) {
        snprintf(output, output_size,
                 "Error: path must start with " MIMI_SPIFFS_BASE "/");
        return NULL;
    }
    return path;
}

esp_err_t tool_lua_check_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *path = get_path(root, output, output_size);
    if (!path) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = mimi_lua_check(path, output, output_size);
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_lua_run_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *path = get_path(root, output, output_size);
    if (!path) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t timeout_ms = 0;
    cJSON *to_obj = cJSON_GetObjectItem(root, "timeout_ms");
    if (cJSON_IsNumber(to_obj) && to_obj->valuedouble > 0) {
        timeout_ms = (uint32_t)to_obj->valuedouble;
    }

    /* "args" may be an object (preferred) or a pre-encoded JSON string. */
    char *args_owned = NULL;
    const char *args_json = NULL;
    cJSON *args_obj = cJSON_GetObjectItem(root, "args");
    if (cJSON_IsObject(args_obj) || cJSON_IsArray(args_obj)) {
        args_owned = cJSON_PrintUnformatted(args_obj);
        args_json = args_owned;
    } else if (cJSON_IsString(args_obj)) {
        args_json = args_obj->valuestring;
    }

    ESP_LOGI(TAG, "lua_run_script: %s (timeout %u ms)", path, (unsigned)timeout_ms);
    esp_err_t err = mimi_lua_run(path, args_json, timeout_ms, output, output_size);

    free(args_owned);
    cJSON_Delete(root);
    return err;
}
