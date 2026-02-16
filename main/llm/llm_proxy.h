#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include <stddef.h>
#include <stdbool.h>

#include "mimi_config.h"

/**
 * Initialize the LLM proxy. Reads API key and model from build-time secrets, then NVS.
 */
esp_err_t llm_proxy_init(void);

/**
 * Save the LLM API key to NVS.
 */
esp_err_t llm_set_api_key(const char *api_key);

/**
 * Save the LLM provider to NVS. (e.g. "anthropic", "openai", "qwen")
 */
esp_err_t llm_set_provider(const char *provider);

/**
 * Save the model identifier to NVS.
 */
esp_err_t llm_set_model(const char *model);

/**
 * Send a chat completion request to the configured LLM API (non-streaming).
 *
 * @param system_prompt  System prompt string
 * @param messages_json  JSON array of messages: [{"role":"user","content":"..."},...]
 * @param response_buf   Output buffer for the complete response text
 * @param buf_size       Size of response_buf
 * @return ESP_OK on success
 */
esp_err_t llm_chat(const char *system_prompt, const char *messages_json,
                   char *response_buf, size_t buf_size);

/* ── Tool Use Support ──────────────────────────────────────────── */

typedef struct {
    char id[64];        /* "toolu_xxx" */
    char name[32];      /* "web_search" */
    char *input;        /* heap-allocated JSON string */
    size_t input_len;
} llm_tool_call_t;

typedef struct {
    char *text;                                  /* accumulated text blocks */
    size_t text_len;
    llm_tool_call_t *calls;                      /* dynamic array of tool calls */
    int call_count;
    int call_capacity;                           /* allocated capacity */
    bool tool_use;                               /* stop_reason == "tool_use" */
} llm_response_t;

void llm_response_free(llm_response_t *resp);

/**
 * Send a chat completion request with tools to the configured LLM API (non-streaming).
 *
 * @param system_prompt  System prompt string
 * @param messages       cJSON array of messages (caller owns)
 * @param tools_json     Pre-built JSON string of tools array, or NULL for no tools
 * @param resp           Output: structured response with text and tool calls
 * @return ESP_OK on success
 */
esp_err_t llm_chat_tools(const char *system_prompt,
                         cJSON *messages,
                         const char *tools_json,
                         llm_response_t *resp);
