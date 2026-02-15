#include "agent_loop.h"
#include "agent/context_builder.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "memory/session_mgr.h"
#include "tools/tool_registry.h"
#include "display/display.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "cJSON.h"

static const char *TAG = "agent";

#define TOOL_OUTPUT_SIZE  (8 * 1024)

/* Build the assistant content array from llm_response_t for the messages history.
 * Returns a cJSON array with text and tool_use blocks. */
static cJSON *build_assistant_content(const llm_response_t *resp)
{
    cJSON *content = cJSON_CreateArray();

    /* Text block */
    if (resp->text && resp->text_len > 0) {
        cJSON *text_block = cJSON_CreateObject();
        cJSON_AddStringToObject(text_block, "type", "text");
        cJSON_AddStringToObject(text_block, "text", resp->text);
        cJSON_AddItemToArray(content, text_block);
    }

    /* Tool use blocks */
    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];
        cJSON *tool_block = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_block, "type", "tool_use");
        cJSON_AddStringToObject(tool_block, "id", call->id);
        cJSON_AddStringToObject(tool_block, "name", call->name);

        cJSON *input = cJSON_Parse(call->input);
        if (input) {
            cJSON_AddItemToObject(tool_block, "input", input);
        } else {
            cJSON_AddItemToObject(tool_block, "input", cJSON_CreateObject());
        }

        cJSON_AddItemToArray(content, tool_block);
    }

    return content;
}

/* Build the user message with tool_result blocks */
static cJSON *build_tool_results(const llm_response_t *resp, char *tool_output, size_t tool_output_size,
                                 char *last_tool_name, size_t last_tool_name_size,
                                 char *last_tool_result, size_t last_tool_result_size)
{
    cJSON *content = cJSON_CreateArray();

    for (int i = 0; i < resp->call_count; i++) {
        const llm_tool_call_t *call = &resp->calls[i];

        /* Execute tool */
        tool_output[0] = '\0';
        display_show_agent_status("[TOOL]", call->name, call->input, true);
        tool_registry_execute(call->name, call->input, tool_output, tool_output_size);
        display_show_agent_status("[TOOL]", call->name, "done", false);

        ESP_LOGI(TAG, "Tool %s result: %d bytes", call->name, (int)strlen(tool_output));
        if (last_tool_name && last_tool_name_size > 0) {
            snprintf(last_tool_name, last_tool_name_size, "%s", call->name);
        }
        if (last_tool_result && last_tool_result_size > 0) {
            snprintf(last_tool_result, last_tool_result_size, "%.180s", tool_output);
        }

        /* Build tool_result block */
        cJSON *result_block = cJSON_CreateObject();
        cJSON_AddStringToObject(result_block, "type", "tool_result");
        cJSON_AddStringToObject(result_block, "tool_use_id", call->id);
        cJSON_AddStringToObject(result_block, "content", tool_output);
        cJSON_AddItemToArray(content, result_block);
    }

    return content;
}

static void agent_loop_task(void *arg)
{
    ESP_LOGI(TAG, "Agent loop started on core %d", xPortGetCoreID());

    /* Allocate large buffers from PSRAM */
    char *system_prompt = heap_caps_calloc(1, MIMI_CONTEXT_BUF_SIZE, MALLOC_CAP_SPIRAM);
    char *history_json = heap_caps_calloc(1, MIMI_LLM_STREAM_BUF_SIZE, MALLOC_CAP_SPIRAM);
    char *tool_output = heap_caps_calloc(1, TOOL_OUTPUT_SIZE, MALLOC_CAP_SPIRAM);

    if (!system_prompt || !history_json || !tool_output) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM buffers");
        vTaskDelete(NULL);
        return;
    }

    const char *tools_json = tool_registry_get_tools_json();

    while (1) {
        mimi_msg_t msg;
        esp_err_t err = message_bus_pop_inbound(&msg, UINT32_MAX);
        if (err != ESP_OK) continue;

        ESP_LOGI(TAG, "Processing message from %s:%s", msg.channel, msg.chat_id);
        ESP_LOGI(TAG, "Stack watermark before request: %u bytes",
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
        display_show_agent_status("[AGENT]", "New Request", msg.content, true);

        /* 1. Build system prompt */
        context_build_system_prompt(system_prompt, MIMI_CONTEXT_BUF_SIZE);

        /* 2. Load session history into cJSON array */
        session_get_history_json(msg.chat_id, history_json,
                                 MIMI_LLM_STREAM_BUF_SIZE, MIMI_AGENT_MAX_HISTORY);

        cJSON *messages = cJSON_Parse(history_json);
        if (!messages) messages = cJSON_CreateArray();

        /* 3. Append current user message */
        cJSON *user_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(user_msg, "role", "user");
        cJSON_AddStringToObject(user_msg, "content", msg.content);
        cJSON_AddItemToArray(messages, user_msg);

        /* 4. ReAct loop */
        char *final_text = NULL;
        int iteration = 0;
        char last_tool_name[32] = {0};
        char last_tool_result[192] = {0};

        while (iteration < MIMI_AGENT_MAX_TOOL_ITER) {
            /* Send "working" indicator before each API call */
            {
                static const char *working_phrases[] = {
                    "mimi\xF0\x9F\x98\x97is working...",
                    "mimi\xF0\x9F\x90\xBE is thinking...",
                    "mimi\xF0\x9F\x92\xAD is pondering...",
                    "mimi\xF0\x9F\x8C\x99 is on it...",
                    "mimi\xE2\x9C\xA8 is cooking...",
                };
                const int phrase_count = sizeof(working_phrases) / sizeof(working_phrases[0]);
                mimi_msg_t status = {0};
                strncpy(status.channel, msg.channel, sizeof(status.channel) - 1);
                strncpy(status.chat_id, msg.chat_id, sizeof(status.chat_id) - 1);
                status.content = strdup(working_phrases[esp_random() % phrase_count]);
                if (status.content) message_bus_push_outbound(&status);
            }

            llm_response_t resp;
            display_show_agent_status("[LLM]", "Thinking", "calling model", true);
            err = llm_chat_tools(system_prompt, messages, tools_json, &resp);
            if (err == ESP_OK) {
                display_show_agent_status("[LLM]", "Response Received", "", false);
            }

            if (err != ESP_OK) {
                ESP_LOGE(TAG, "LLM call failed: %s", esp_err_to_name(err));
                if (err == ESP_ERR_INVALID_STATE) {
                    free(final_text);
                    final_text = strdup("LLM authentication failed. Please run `set_api_key <YOUR_VALID_KEY>` and retry.");
                }
                break;
            }

            if (!resp.tool_use) {
                /* Normal completion — save final text and break */
                if (resp.text && resp.text_len > 0) {
                    final_text = strdup(resp.text);
                }
                llm_response_free(&resp);
                break;
            }

            ESP_LOGI(TAG, "Tool use iteration %d: %d calls", iteration + 1, resp.call_count);

            /* Append assistant message with content array */
            cJSON *asst_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(asst_msg, "role", "assistant");
            cJSON_AddItemToObject(asst_msg, "content", build_assistant_content(&resp));
            cJSON_AddItemToArray(messages, asst_msg);

            /* Execute tools and append results */
            cJSON *tool_results = build_tool_results(
                &resp, tool_output, TOOL_OUTPUT_SIZE,
                last_tool_name, sizeof(last_tool_name),
                last_tool_result, sizeof(last_tool_result));
            cJSON *result_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(result_msg, "role", "user");
            cJSON_AddItemToObject(result_msg, "content", tool_results);
            cJSON_AddItemToArray(messages, result_msg);

            llm_response_free(&resp);
            iteration++;
        }

        if (!final_text && iteration >= MIMI_AGENT_MAX_TOOL_ITER) {
            ESP_LOGW(TAG, "Reached max tool iterations (%d), forcing text completion", MIMI_AGENT_MAX_TOOL_ITER);

            char *messages_json = cJSON_PrintUnformatted(messages);
            char *fallback_text = heap_caps_calloc(1, 2048, MALLOC_CAP_SPIRAM);
            if (!fallback_text) {
                fallback_text = calloc(1, 2048);
            }
            if (messages_json && fallback_text) {
                err = llm_chat(system_prompt, messages_json, fallback_text, 2048);
                if (err == ESP_OK && fallback_text[0]) {
                    final_text = strdup(fallback_text);
                }
            }
            free(messages_json);
            free(fallback_text);

            if (!final_text) {
                if (last_tool_name[0]) {
                    char msg_buf[320];
                    snprintf(msg_buf, sizeof(msg_buf),
                             "Tool loop reached limit (%d). Last tool `%s` output: %s",
                             MIMI_AGENT_MAX_TOOL_ITER, last_tool_name,
                             last_tool_result[0] ? last_tool_result : "(empty)");
                    final_text = strdup(msg_buf);
                } else {
                    final_text = strdup("Tool loop reached limit and no final answer was generated.");
                }
            }
        }

        cJSON_Delete(messages);

        /* 5. Send response */
        if (final_text && final_text[0]) {
            /* Save to session (only user text + final assistant text) */
            session_append(msg.chat_id, "user", msg.content);
            session_append(msg.chat_id, "assistant", final_text);
            display_show_agent_status("[DONE]", "Reply Ready", final_text, false);
            ESP_LOGI(TAG, "Final reply (%d bytes): %.320s", (int)strlen(final_text), final_text);

            /* Push response to outbound */
            mimi_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = final_text;  /* transfer ownership */
            message_bus_push_outbound(&out);
        } else {
            /* Error or empty response */
            free(final_text);
            display_show_agent_status("[ERR]", "Agent Error", "Sorry, I encountered an error.", false);
            ESP_LOGW(TAG, "Final reply empty, sending generic error");
            mimi_msg_t out = {0};
            strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
            out.content = strdup("Sorry, I encountered an error.");
            if (out.content) {
                message_bus_push_outbound(&out);
            }
        }
        display_clear_agent_status();

        /* Free inbound message content */
        free(msg.content);

        /* Log memory status */
        ESP_LOGI(TAG, "Free PSRAM: %d bytes",
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        ESP_LOGI(TAG, "Stack watermark after request: %u bytes",
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    }
}

esp_err_t agent_loop_init(void)
{
    ESP_LOGI(TAG, "Agent loop initialized");
    return ESP_OK;
}

esp_err_t agent_loop_start(void)
{
    static StaticTask_t s_agent_task_tcb;
    static StackType_t s_agent_task_stack[MIMI_AGENT_STACK / sizeof(StackType_t)];
    static TaskHandle_t s_agent_task_handle = NULL;

    if (s_agent_task_handle != NULL) {
        return ESP_OK;
    }

    s_agent_task_handle = xTaskCreateStaticPinnedToCore(
        agent_loop_task, "agent_loop",
        MIMI_AGENT_STACK / sizeof(StackType_t),
        NULL,
        MIMI_AGENT_PRIO,
        s_agent_task_stack,
        &s_agent_task_tcb,
        MIMI_AGENT_CORE);
    if (s_agent_task_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create agent loop task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Agent loop task created with static stack=%u bytes", (unsigned)MIMI_AGENT_STACK);
    return ESP_OK;
}
