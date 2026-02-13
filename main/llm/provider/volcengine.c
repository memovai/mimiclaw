#include "llm_provider.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"



static const char *TAG = "provider_volcengine";

static char s_model[64] = "doubao-seed-1-8-251228";

cJSON *convert_messages_volc(const char *system_prompt, cJSON *messages)
{
    cJSON *out = cJSON_CreateArray();

    if (system_prompt && system_prompt[0]) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", system_prompt);
        cJSON_AddItemToArray(out, sys);
    }

    if (!messages || !cJSON_IsArray(messages)) return out;

    cJSON *msg;
    cJSON_ArrayForEach(msg, messages) {
        cJSON *role = cJSON_GetObjectItem(msg, "role");
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (!role || !cJSON_IsString(role)) continue;

        if (strcmp(role->valuestring, "assistant") == 0 && content && cJSON_IsArray(content)) {
            cJSON *block;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (!btype || !cJSON_IsString(btype)) continue;

                if (strcmp(btype->valuestring, "tool_use") == 0) {
                    cJSON *id = cJSON_GetObjectItem(block, "id");
                    cJSON *name = cJSON_GetObjectItem(block, "name");
                    cJSON *input = cJSON_GetObjectItem(block, "input");

                    cJSON *fc = cJSON_CreateObject();
                    cJSON_AddStringToObject(fc, "type", "function_call");
                    if (id && cJSON_IsString(id)) {
                        cJSON_AddStringToObject(fc, "call_id", id->valuestring);
                    }
                    if (name && cJSON_IsString(name)) {
                        cJSON_AddStringToObject(fc, "name", name->valuestring);
                    }
                    if (input) {
                        char *input_str = cJSON_PrintUnformatted(input);
                        if (input_str) {
                            cJSON_AddStringToObject(fc, "arguments", input_str);
                            free(input_str);
                        }
                    }
                    cJSON_AddItemToArray(out, fc);
                }
            }
        } else if (strcmp(role->valuestring, "user") == 0 && content && cJSON_IsArray(content)) {
            cJSON *block;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (!btype || !cJSON_IsString(btype)) continue;

                if (strcmp(btype->valuestring, "tool_result") == 0) {
                    cJSON *tool_id = cJSON_GetObjectItem(block, "tool_use_id");
                    cJSON *tcontent = cJSON_GetObjectItem(block, "content");

                    cJSON *fco = cJSON_CreateObject();
                    cJSON_AddStringToObject(fco, "type", "function_call_output");
                    if (tool_id && cJSON_IsString(tool_id)) {
                        cJSON_AddStringToObject(fco, "call_id", tool_id->valuestring);
                    }
                    if (tcontent && cJSON_IsString(tcontent)) {
                        cJSON_AddStringToObject(fco, "output", tcontent->valuestring);
                    }
                    cJSON_AddItemToArray(out, fco);
                } else if (strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        cJSON *m = cJSON_CreateObject();
                        cJSON_AddStringToObject(m, "role", "user");
                        cJSON_AddStringToObject(m, "content", text->valuestring);
                        cJSON_AddItemToArray(out, m);
                    }
                }
            }
        } else {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", role->valuestring);

            if (content) {
                if (cJSON_IsString(content)) {
                    cJSON_AddStringToObject(m, "content", content->valuestring);
                } else if (cJSON_IsArray(content)) {
                    cJSON *content_arr = cJSON_CreateArray();
                    cJSON *block;
                    cJSON_ArrayForEach(block, content) {
                        cJSON *btype = cJSON_GetObjectItem(block, "type");
                        if (!btype || !cJSON_IsString(btype)) continue;

                        if (strcmp(btype->valuestring, "text") == 0) {
                            cJSON *text = cJSON_GetObjectItem(block, "text");
                            if (text && cJSON_IsString(text)) {
                                cJSON *mb = cJSON_CreateObject();
                                cJSON_AddStringToObject(mb, "type", "input_text");
                                cJSON_AddStringToObject(mb, "text", text->valuestring);
                                cJSON_AddItemToArray(content_arr, mb);
                            }
                        }
                    }
                    if (cJSON_GetArraySize(content_arr) > 0) {
                        cJSON_AddItemToObject(m, "content", content_arr);
                    } else {
                        cJSON_Delete(content_arr);
                    }
                }
            }
            cJSON_AddItemToArray(out, m);
        }
    }

    return out;
}

static esp_err_t volcengine_build_request(const llm_provider_t *provider,
                                          const char *system_prompt,
                                          cJSON *messages,
                                          const char *tools_json,
                                          char *out_buf, size_t out_size)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", s_model);
    cJSON_AddNumberToObject(body, "max_output_tokens", MIMI_LLM_MAX_TOKENS);

    cJSON *msgs = convert_messages_volc(system_prompt, messages);
    cJSON_AddItemToObject(body, "input", msgs);

    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    size_t len = strlen(json_str);
    if (len >= out_size) {
        free(json_str);
        return ESP_ERR_NO_MEM;
    }

    memcpy(out_buf, json_str, len + 1);
    free(json_str);
    return ESP_OK;
}

static esp_err_t volcengine_parse_response(const llm_provider_t *provider,
                                           const char *response_body,
                                           char *text_out, size_t text_size,
                                           int *stop_reason)
{
    text_out[0] = '\0';
    if (stop_reason) *stop_reason = 0;

    cJSON *root = cJSON_Parse(response_body);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_FAIL;
    }

    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (status && cJSON_IsString(status)) {
        ESP_LOGI(TAG, "Response status: %s", status->valuestring);
        if (strcmp(status->valuestring, "completed") == 0 && stop_reason) {
            *stop_reason = 1;
        }
    }

    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (error) {
        cJSON *err_msg = cJSON_GetObjectItem(error, "message");
        if (err_msg && cJSON_IsString(err_msg)) {
            ESP_LOGE(TAG, "API error: %s", err_msg->valuestring);
        }
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *output = cJSON_GetObjectItem(root, "output");
    if (!output || !cJSON_IsArray(output)) {
        ESP_LOGE(TAG, "No output array in response");
        ESP_LOGI(TAG, "Response body: %.500s", response_body);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Output array has %d items", cJSON_GetArraySize(output));

    size_t off = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, output) {
        cJSON *type = cJSON_GetObjectItem(item, "type");
        if (!type || !cJSON_IsString(type)) continue;

        ESP_LOGI(TAG, "Output item type: %s", type->valuestring);

        if (strcmp(type->valuestring, "message") == 0) {
            cJSON *content = cJSON_GetObjectItem(item, "content");
            if (content && cJSON_IsArray(content)) {
                cJSON *block;
                cJSON_ArrayForEach(block, content) {
                    cJSON *btype = cJSON_GetObjectItem(block, "type");
                    if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "output_text") == 0) {
                        cJSON *text = cJSON_GetObjectItem(block, "text");
                        if (text && cJSON_IsString(text)) {
                            size_t tlen = strlen(text->valuestring);
                            if (off + tlen < text_size) {
                                memcpy(text_out + off, text->valuestring, tlen);
                                off += tlen;
                            }
                        }
                    }
                }
            }
        } else if (strcmp(type->valuestring, "function_call") == 0) {
            ESP_LOGI(TAG, "Found function_call in response");
        }
    }

    text_out[off] = '\0';
    ESP_LOGI(TAG, "Parsed text length: %d", (int)off);

    cJSON_Delete(root);
    return ESP_OK;
}

static void volcengine_set_auth(const llm_provider_t *provider, void *http_client)
{
}

static llm_provider_t volcengine_provider;

esp_err_t provider_volcengine_init(void)
{
    memset(&volcengine_provider, 0, sizeof(volcengine_provider));

    strncpy(volcengine_provider.name, "volcengine", sizeof(volcengine_provider.name) - 1);

    volcengine_provider.config.name = "volcengine";
    volcengine_provider.config.api_url = MIMI_VOLCENGINE_API_URL;
    volcengine_provider.config.api_path = MIMI_VOLCENGINE_API_PATH;
    volcengine_provider.config.auth_header = "Authorization";
    volcengine_provider.config.extra_header_key = NULL;
    volcengine_provider.config.extra_header_value = NULL;

    volcengine_provider.ops.build_request = volcengine_build_request;
    volcengine_provider.ops.parse_response = volcengine_parse_response;
    volcengine_provider.ops.set_auth = volcengine_set_auth;

    volcengine_provider.user_data = NULL;

    return llm_provider_register(&volcengine_provider);
}

void provider_volcengine_set_model(const char *model)
{
    if (model) {
        strncpy(s_model, model, sizeof(s_model) - 1);
    }
}
