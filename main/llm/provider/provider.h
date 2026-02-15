#pragma once

#include "esp_err.h"
#include "cJSON.h"

esp_err_t provider_anthropic_init(void);
void provider_anthropic_set_model(const char *model);

esp_err_t provider_openai_init(void);
void provider_openai_set_model(const char *model);

esp_err_t provider_volcengine_init(void);
void provider_volcengine_set_model(const char *model);

cJSON *convert_messages_volc(const char *system_prompt, cJSON *messages);
