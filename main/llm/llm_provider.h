#pragma once

#include "esp_err.h"
#include "cJSON.h"

#define LLM_PROVIDER_NAME_MAX  16

typedef struct llm_provider llm_provider_t;

typedef struct {
    const char *name;
    const char *api_url;
    const char *api_path;
    const char *auth_header;
    const char *extra_header_key;
    const char *extra_header_value;
} llm_provider_config_t;

typedef struct {
    esp_err_t (*build_request)(const llm_provider_t *provider,
                               const char *system_prompt,
                               cJSON *messages,
                               const char *tools_json,
                               char *out_buf, size_t out_size);

    esp_err_t (*parse_response)(const llm_provider_t *provider,
                                const char *response_body,
                                char *text_out, size_t text_size,
                                int *stop_reason);

    void (*set_auth)(const llm_provider_t *provider,
                    void *http_client);
} llm_provider_ops_t;

struct llm_provider {
    char name[LLM_PROVIDER_NAME_MAX];
    llm_provider_config_t config;
    llm_provider_ops_t ops;
    void *user_data;
};

esp_err_t llm_provider_register(const llm_provider_t *provider);
const llm_provider_t *llm_provider_get(const char *name);
const llm_provider_t *llm_provider_get_default(void);
void llm_provider_set_default(const char *name);
