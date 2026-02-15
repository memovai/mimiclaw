#include "llm_provider.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"



static char s_model[64] = MIMI_LLM_DEFAULT_MODEL;

static esp_err_t anthropic_build_request(const llm_provider_t *provider,
                                         const char *system_prompt,
                                         cJSON *messages,
                                         const char *tools_json,
                                         char *out_buf, size_t out_size)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", s_model);
    cJSON_AddNumberToObject(body, "max_tokens", MIMI_LLM_MAX_TOKENS);

    if (system_prompt && system_prompt[0]) {
        cJSON_AddStringToObject(body, "system", system_prompt);
    }

    if (messages && cJSON_IsArray(messages)) {
        cJSON *msgs_copy = cJSON_Duplicate(messages, 1);
        cJSON_AddItemToObject(body, "messages", msgs_copy);
    } else {
        cJSON *arr = cJSON_CreateArray();
        cJSON *msg = cJSON_CreateObject();
        cJSON_AddStringToObject(msg, "role", "user");
        cJSON_AddStringToObject(msg, "content", "");
        cJSON_AddItemToArray(arr, msg);
        cJSON_AddItemToObject(body, "messages", arr);
    }

    if (tools_json) {
        cJSON *tools = cJSON_Parse(tools_json);
        if (tools) {
            cJSON_AddItemToObject(body, "tools", tools);
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

static esp_err_t anthropic_parse_response(const llm_provider_t *provider,
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

    cJSON *stop = cJSON_GetObjectItem(root, "stop_reason");
    if (stop && cJSON_IsString(stop)) {
        if (stop_reason) {
            if (strcmp(stop->valuestring, "end_turn") == 0) *stop_reason = 1;
            else if (strcmp(stop->valuestring, "tool_use") == 0) *stop_reason = 2;
            else if (strcmp(stop->valuestring, "max_tokens") == 0) *stop_reason = 3;
        }
    }

    cJSON *content = cJSON_GetObjectItem(root, "content");
    if (!content || !cJSON_IsArray(content)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t off = 0;
    cJSON *block;
    cJSON_ArrayForEach(block, content) {
        cJSON *btype = cJSON_GetObjectItem(block, "type");
        if (!btype || strcmp(btype->valuestring, "text") != 0) continue;
        cJSON *text = cJSON_GetObjectItem(block, "text");
        if (!text || !cJSON_IsString(text)) continue;

        size_t tlen = strlen(text->valuestring);
        if (off + tlen < text_size) {
            memcpy(text_out + off, text->valuestring, tlen);
            off += tlen;
        }
    }
    text_out[off] = '\0';

    cJSON_Delete(root);
    return ESP_OK;
}

static void anthropic_set_auth(const llm_provider_t *provider, void *http_client)
{
}

static llm_provider_t anthropic_provider;

esp_err_t provider_anthropic_init(void)
{
    memset(&anthropic_provider, 0, sizeof(anthropic_provider));

    strncpy(anthropic_provider.name, "anthropic", sizeof(anthropic_provider.name) - 1);

    anthropic_provider.config.name = "anthropic";
    anthropic_provider.config.api_url = MIMI_LLM_API_URL;
    anthropic_provider.config.api_path = "/v1/messages";
    anthropic_provider.config.auth_header = "Authorization";
    anthropic_provider.config.extra_header_key = "x-api-key";
    anthropic_provider.config.extra_header_value = NULL;

    anthropic_provider.ops.build_request = anthropic_build_request;
    anthropic_provider.ops.parse_response = anthropic_parse_response;
    anthropic_provider.ops.set_auth = anthropic_set_auth;

    anthropic_provider.user_data = NULL;

    return llm_provider_register(&anthropic_provider);
}

void provider_anthropic_set_model(const char *model)
{
    if (model) {
        strncpy(s_model, model, sizeof(s_model) - 1);
    }
}
