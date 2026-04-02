#pragma once

#include <stddef.h>

#include "esp_err.h"

esp_err_t llm_proxy_init(void);

const char *llm_proxy_provider(void);
const char *llm_proxy_model(void);
const char *llm_proxy_endpoint(void);

esp_err_t llm_proxy_chat_once(const char *system_prompt,
                              const char *user_question,
                              char *answer,
                              size_t answer_size,
                              int *http_status,
                              char *error,
                              size_t error_size);
