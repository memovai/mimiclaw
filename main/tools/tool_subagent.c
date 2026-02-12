#include "tools/tool_subagent.h"
#include "tools/tool_registry.h"
#include "agent/context_builder.h"
#include "llm/llm_proxy.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

static const char *TAG = "subagent";

/* Filtered tools JSON (all tools except spawn_subagent) */
static char *s_subagent_tools_json = NULL;

typedef struct {
    char *task;
    char *context;
    char *result;
    SemaphoreHandle_t done_sem;
    bool timeout_abandoned;
} subagent_ctx_t;

/* ── Build assistant content (duplicated from agent_loop.c, static there) ── */

static cJSON *build_assistant_content(const llm_response_t *resp)
{
    cJSON *content = cJSON_CreateArray();

    if (resp->text && resp->text_len > 0) {
        cJSON *text_block = cJSON_CreateObject();
        cJSON_AddStringToObject(text_block, "type", "text");
        cJSON_AddStringToObject(text_block, "text", resp->text);
        cJSON_AddItemToArray(content, text_block);
    }

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

/* ── Subagent FreeRTOS task ── */

static void subagent_task(void *arg)
{
    subagent_ctx_t *ctx = (subagent_ctx_t *)arg;

    ESP_LOGI(TAG, "Subagent started: %.60s...", ctx->task);

    /* Allocate buffers from PSRAM */
    char *system_prompt = heap_caps_calloc(1, MIMI_SUBAGENT_CTX_SIZE, MALLOC_CAP_SPIRAM);
    char *tool_output = heap_caps_calloc(1, MIMI_SUBAGENT_TOOL_BUF, MALLOC_CAP_SPIRAM);

    if (!system_prompt || !tool_output) {
        ESP_LOGE(TAG, "Subagent PSRAM alloc failed");
        ctx->result = strdup("Error: subagent memory allocation failed");
        goto done;
    }

    /* Build system prompt */
    context_build_system_prompt(system_prompt, MIMI_SUBAGENT_CTX_SIZE);

    /* Append subagent-specific instruction */
    size_t used = strlen(system_prompt);
    size_t remaining = MIMI_SUBAGENT_CTX_SIZE - used;
    snprintf(system_prompt + used, remaining,
             "\n\n--- SUBAGENT MODE ---\n"
             "You are a subagent spawned to handle a specific subtask. "
             "Complete the task and provide your final answer as concise text. "
             "You cannot spawn further subagents.\n");

    /* Append optional context */
    if (ctx->context && ctx->context[0]) {
        used = strlen(system_prompt);
        remaining = MIMI_SUBAGENT_CTX_SIZE - used;
        snprintf(system_prompt + used, remaining,
                 "\nAdditional context:\n%s\n", ctx->context);
    }

    /* Create messages array with single user message */
    cJSON *messages = cJSON_CreateArray();
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", ctx->task);
    cJSON_AddItemToArray(messages, user_msg);

    /* ReAct loop */
    for (int iter = 0; iter < MIMI_SUBAGENT_MAX_TOOL_ITER; iter++) {
        llm_response_t resp;
        esp_err_t err = llm_chat_tools(system_prompt, messages,
                                        s_subagent_tools_json, &resp);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Subagent LLM call failed: %s", esp_err_to_name(err));
            ctx->result = strdup("Error: subagent LLM call failed");
            break;
        }

        if (!resp.tool_use) {
            /* Final answer */
            if (resp.text && resp.text_len > 0) {
                ctx->result = strdup(resp.text);
            } else {
                ctx->result = strdup("(subagent produced no output)");
            }
            llm_response_free(&resp);
            break;
        }

        ESP_LOGI(TAG, "Subagent tool iter %d: %d calls", iter + 1, resp.call_count);

        /* Append assistant message */
        cJSON *asst_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(asst_msg, "role", "assistant");
        cJSON_AddItemToObject(asst_msg, "content", build_assistant_content(&resp));
        cJSON_AddItemToArray(messages, asst_msg);

        /* Execute tools and append results */
        cJSON *results = cJSON_CreateArray();
        for (int i = 0; i < resp.call_count; i++) {
            const llm_tool_call_t *call = &resp.calls[i];
            tool_output[0] = '\0';
            tool_registry_execute(call->name, call->input,
                                  tool_output, MIMI_SUBAGENT_TOOL_BUF);
            ESP_LOGI(TAG, "Subagent tool %s: %d bytes", call->name, (int)strlen(tool_output));

            cJSON *result_block = cJSON_CreateObject();
            cJSON_AddStringToObject(result_block, "type", "tool_result");
            cJSON_AddStringToObject(result_block, "tool_use_id", call->id);
            cJSON_AddStringToObject(result_block, "content", tool_output);
            cJSON_AddItemToArray(results, result_block);
        }

        cJSON *result_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(result_msg, "role", "user");
        cJSON_AddItemToObject(result_msg, "content", results);
        cJSON_AddItemToArray(messages, result_msg);

        llm_response_free(&resp);
    }

    /* If loop exhausted without setting result */
    if (!ctx->result) {
        ctx->result = strdup("Error: subagent reached max iterations without final answer");
    }

    cJSON_Delete(messages);

done:
    free(system_prompt);
    free(tool_output);

    ESP_LOGI(TAG, "Subagent done, result: %d bytes",
             ctx->result ? (int)strlen(ctx->result) : 0);

    xSemaphoreGive(ctx->done_sem);

    /* If caller already timed out, we own cleanup */
    if (ctx->timeout_abandoned) {
        ESP_LOGW(TAG, "Subagent: caller timed out, cleaning up");
        vSemaphoreDelete(ctx->done_sem);
        free(ctx->task);
        free(ctx->context);
        free(ctx->result);
        free(ctx);
    }

    vTaskDelete(NULL);
}

/* ── Public API ── */

esp_err_t tool_subagent_init(void)
{
    /* Build filtered tools JSON: all tools except spawn_subagent */
    const mimi_tool_t *tools;
    int count;
    tool_registry_get_tools(&tools, &count);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        if (strcmp(tools[i].name, "spawn_subagent") == 0) {
            continue;
        }

        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", tools[i].name);
        cJSON_AddStringToObject(tool, "description", tools[i].description);

        cJSON *schema = cJSON_Parse(tools[i].input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }

        cJSON_AddItemToArray(arr, tool);
    }

    free(s_subagent_tools_json);
    s_subagent_tools_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Subagent tools JSON built (excluding spawn_subagent)");
    return ESP_OK;
}

esp_err_t tool_subagent_execute(const char *input_json, char *output, size_t output_size)
{
    /* Parse input */
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *task_json = cJSON_GetObjectItem(input, "task");
    if (!task_json || !cJSON_IsString(task_json)) {
        snprintf(output, output_size, "Error: 'task' field is required");
        cJSON_Delete(input);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *context_json = cJSON_GetObjectItem(input, "context");

    /* Allocate context struct in PSRAM */
    subagent_ctx_t *ctx = heap_caps_calloc(1, sizeof(subagent_ctx_t), MALLOC_CAP_SPIRAM);
    if (!ctx) {
        snprintf(output, output_size, "Error: failed to allocate subagent context");
        cJSON_Delete(input);
        return ESP_ERR_NO_MEM;
    }

    ctx->task = strdup(task_json->valuestring);
    ctx->context = (context_json && cJSON_IsString(context_json))
                   ? strdup(context_json->valuestring) : NULL;
    ctx->result = NULL;
    ctx->timeout_abandoned = false;
    ctx->done_sem = xSemaphoreCreateBinary();

    cJSON_Delete(input);

    if (!ctx->task || !ctx->done_sem) {
        free(ctx->task);
        free(ctx->context);
        if (ctx->done_sem) vSemaphoreDelete(ctx->done_sem);
        free(ctx);
        snprintf(output, output_size, "Error: subagent setup failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Spawning subagent for: %.60s...", ctx->task);

    /* Spawn the subagent task */
    BaseType_t ret = xTaskCreatePinnedToCore(
        subagent_task, "subagent",
        MIMI_SUBAGENT_STACK, ctx,
        MIMI_SUBAGENT_PRIO, NULL, MIMI_SUBAGENT_CORE);

    if (ret != pdPASS) {
        vSemaphoreDelete(ctx->done_sem);
        free(ctx->task);
        free(ctx->context);
        free(ctx);
        snprintf(output, output_size, "Error: failed to create subagent task");
        return ESP_FAIL;
    }

    /* Block until subagent completes or timeout */
    TickType_t timeout_ticks = pdMS_TO_TICKS(MIMI_SUBAGENT_TIMEOUT_MS);
    BaseType_t got = xSemaphoreTake(ctx->done_sem, timeout_ticks);

    if (got != pdTRUE) {
        /* Timeout — tell the subagent task it owns cleanup */
        ESP_LOGW(TAG, "Subagent timed out after %d ms", MIMI_SUBAGENT_TIMEOUT_MS);
        ctx->timeout_abandoned = true;
        snprintf(output, output_size, "Error: subagent timed out after %d seconds",
                 MIMI_SUBAGENT_TIMEOUT_MS / 1000);
        return ESP_ERR_TIMEOUT;
    }

    /* Copy result to output */
    if (ctx->result) {
        snprintf(output, output_size, "%s", ctx->result);
    } else {
        snprintf(output, output_size, "(subagent returned no result)");
    }

    /* Cleanup */
    vSemaphoreDelete(ctx->done_sem);
    free(ctx->task);
    free(ctx->context);
    free(ctx->result);
    free(ctx);

    ESP_LOGI(TAG, "Subagent completed, output: %d bytes", (int)strlen(output));
    return ESP_OK;
}
