#include "llm_provider.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"



static char s_model[64] = "gpt-4o";

static cJSON *convert_messages_openai(const char *system_prompt, cJSON *messages)
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

        if (content && cJSON_IsString(content)) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", role->valuestring);
            cJSON_AddStringToObject(m, "content", content->valuestring);
            cJSON_AddItemToArray(out, m);
            continue;
        }

        if (!content || !cJSON_IsArray(content)) continue;

        if (strcmp(role->valuestring, "assistant") == 0) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", "assistant");

            char *text_buf = NULL;
            size_t off = 0;
            cJSON *block;
            cJSON *tool_calls = NULL;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        size_t tlen = strlen(text->valuestring);
                        char *tmp = realloc(text_buf, off + tlen + 1);
                        if (tmp) {
                            text_buf = tmp;
                            memcpy(text_buf + off, text->valuestring, tlen);
                            off += tlen;
                            text_buf[off] = '\0';
                        }
                    }
                } else if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "tool_use") == 0) {
                    if (!tool_calls) tool_calls = cJSON_CreateArray();
                    cJSON *id = cJSON_GetObjectItem(block, "id");
                    cJSON *name = cJSON_GetObjectItem(block, "name");
                    cJSON *input = cJSON_GetObjectItem(block, "input");
                    if (!name || !cJSON_IsString(name)) continue;

                    cJSON *tc = cJSON_CreateObject();
                    if (id && cJSON_IsString(id)) {
                        cJSON_AddStringToObject(tc, "id", id->valuestring);
                    }
                    cJSON_AddStringToObject(tc, "type", "function");
                    cJSON *func = cJSON_CreateObject();
                    cJSON_AddStringToObject(func, "name", name->valuestring);
                    if (input) {
                        char *args = cJSON_PrintUnformatted(input);
                        if (args) {
                            cJSON_AddStringToObject(func, "arguments", args);
                            free(args);
                        }
                    }
                    cJSON_AddItemToObject(tc, "function", func);
                    cJSON_AddItemToArray(tool_calls, tc);
                }
            }
            if (text_buf) {
                cJSON_AddStringToObject(m, "content", text_buf);
            } else {
                cJSON_AddStringToObject(m, "content", "");
            }
            if (tool_calls) {
                cJSON_AddItemToObject(m, "tool_calls", tool_calls);
            }
            cJSON_AddItemToArray(out, m);
            free(text_buf);
        } else if (strcmp(role->valuestring, "user") == 0) {
            cJSON *block;
            bool has_user_text = false;
            char *text_buf = NULL;
            size_t off = 0;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "tool_result") == 0) {
                    cJSON *tool_id = cJSON_GetObjectItem(block, "tool_use_id");
                    cJSON *tcontent = cJSON_GetObjectItem(block, "content");
                    if (!tool_id || !cJSON_IsString(tool_id)) continue;
                    cJSON *tm = cJSON_CreateObject();
                    cJSON_AddStringToObject(tm, "role", "tool");
                    cJSON_AddStringToObject(tm, "tool_call_id", tool_id->valuestring);
                    if (tcontent && cJSON_IsString(tcontent)) {
                        cJSON_AddStringToObject(tm, "content", tcontent->valuestring);
                    } else {
                        cJSON_AddStringToObject(tm, "content", "");
                    }
                    cJSON_AddItemToArray(out, tm);
                } else if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        size_t tlen = strlen(text->valuestring);
                        char *tmp = realloc(text_buf, off + tlen + 1);
                        if (tmp) {
                            text_buf = tmp;
                            memcpy(text_buf + off, text->valuestring, tlen);
                            off += tlen;
                            text_buf[off] = '\0';
                        }
                        has_user_text = true;
                    }
                }
            }
            if (has_user_text) {
                cJSON *um = cJSON_CreateObject();
                cJSON_AddStringToObject(um, "role", "user");
                cJSON_AddStringToObject(um, "content", text_buf);
                cJSON_AddItemToArray(out, um);
            }
            free(text_buf);
        }
    }

    return out;
}

static cJSON *convert_tools_openai(const char *tools_json)
{
    if (!tools_json) return NULL;
    cJSON *arr = cJSON_Parse(tools_json);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return NULL;
    }
    cJSON *out = cJSON_CreateArray();
    cJSON *tool;
    cJSON_ArrayForEach(tool, arr) {
        cJSON *name = cJSON_GetObjectItem(tool, "name");
        cJSON *desc = cJSON_GetObjectItem(tool, "description");
        cJSON *schema = cJSON_GetObjectItem(tool, "input_schema");
        if (!name || !cJSON_IsString(name)) continue;

        cJSON *func = cJSON_CreateObject();
        cJSON_AddStringToObject(func, "name", name->valuestring);
        if (desc && cJSON_IsString(desc)) {
            cJSON_AddStringToObject(func, "description", desc->valuestring);
        }
        if (schema) {
            cJSON_AddItemToObject(func, "parameters", cJSON_Duplicate(schema, 1));
        }

        cJSON *wrap = cJSON_CreateObject();
        cJSON_AddStringToObject(wrap, "type", "function");
        cJSON_AddItemToObject(wrap, "function", func);
        cJSON_AddItemToArray(out, wrap);
    }
    cJSON_Delete(arr);
    return out;
}

static esp_err_t openai_build_request(const llm_provider_t *provider,
                                       const char *system_prompt,
                                       cJSON *messages,
                                       const char *tools_json,
                                       char *out_buf, size_t out_size)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", s_model);

    cJSON *openai_msgs = convert_messages_openai(system_prompt, messages);
    cJSON_AddItemToObject(body, "messages", openai_msgs);

    if (tools_json) {
        cJSON *tools = convert_tools_openai(tools_json);
        if (tools) {
            cJSON_AddItemToObject(body, "tools", tools);
            cJSON_AddStringToObject(body, "tool_choice", "auto");
        }
    }

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

static esp_err_t openai_parse_response(const llm_provider_t *provider,
                                       const char *response_body,
                                       char *text_out, size_t text_size,
                                       int *stop_reason)
{
    text_out[0] = '\0';
    if (stop_reason) *stop_reason = 0;

    cJSON *root = cJSON_Parse(response_body);
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!choices || !cJSON_IsArray(choices)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
    if (!choice0) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *finish = cJSON_GetObjectItem(choice0, "finish_reason");
    if (finish && cJSON_IsString(finish)) {
        if (stop_reason) {
            if (strcmp(finish->valuestring, "stop") == 0) *stop_reason = 1;
            else if (strcmp(finish->valuestring, "tool_calls") == 0) *stop_reason = 2;
            else if (strcmp(finish->valuestring, "length") == 0) *stop_reason = 3;
        }
    }

    cJSON *message = cJSON_GetObjectItem(choice0, "message");
    if (message) {
        cJSON *content = cJSON_GetObjectItem(message, "content");
        if (content && cJSON_IsString(content)) {
            strncpy(text_out, content->valuestring, text_size - 1);
            text_out[text_size - 1] = '\0';
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static void openai_set_auth(const llm_provider_t *provider, void *http_client)
{
}

static llm_provider_t openai_provider;

esp_err_t provider_openai_init(void)
{
    memset(&openai_provider, 0, sizeof(openai_provider));

    strncpy(openai_provider.name, "openai", sizeof(openai_provider.name) - 1);

    openai_provider.config.name = "openai";
    openai_provider.config.api_url = MIMI_OPENAI_API_URL;
    openai_provider.config.api_path = "/v1/chat/completions";
    openai_provider.config.auth_header = "Authorization";
    openai_provider.config.extra_header_key = NULL;
    openai_provider.config.extra_header_value = NULL;

    openai_provider.ops.build_request = openai_build_request;
    openai_provider.ops.parse_response = openai_parse_response;
    openai_provider.ops.set_auth = openai_set_auth;

    openai_provider.user_data = NULL;

    return llm_provider_register(&openai_provider);
}

void provider_openai_set_model(const char *model)
{
    if (model) {
        strncpy(s_model, model, sizeof(s_model) - 1);
    }
}
