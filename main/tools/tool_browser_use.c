#include "tools/tool_browser_use.h"

#include "gateway/ws_server.h"
#include "display/display.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_browser";
static const size_t BROWSER_RPC_BUF_SIZE = 4096;

static const char *extract_cmd(const cJSON *root)
{
    cJSON *cmd = cJSON_GetObjectItem((cJSON *)root, "command");
    if (cmd && cJSON_IsString(cmd) && cmd->valuestring[0]) return cmd->valuestring;
    cJSON *type = cJSON_GetObjectItem((cJSON *)root, "type");
    if (type && cJSON_IsString(type) && type->valuestring[0]) return type->valuestring;
    return NULL;
}

static bool str_contains(const char *haystack, const char *needle)
{
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static bool is_twitter_url(const char *url)
{
    if (!url) return false;
    return str_contains(url, "://x.com") || str_contains(url, "://twitter.com");
}

static bool parse_dom_is_twitter(const char *dom_json)
{
    if (!dom_json || !dom_json[0]) return false;
    cJSON *root = cJSON_Parse(dom_json);
    if (!root) return false;
    cJSON *url = cJSON_GetObjectItem(root, "url");
    bool yes = (url && cJSON_IsString(url) && is_twitter_url(url->valuestring));
    cJSON_Delete(root);
    return yes;
}

static bool parse_twitter_fill_ready(const char *dom_json)
{
    if (!dom_json || !dom_json[0]) return false;
    cJSON *root = cJSON_Parse(dom_json);
    if (!root) return false;

    cJSON *compose = cJSON_GetObjectItem(root, "twitterCompose");
    cJSON *draft_len = compose ? cJSON_GetObjectItem(compose, "draftLength") : NULL;
    cJSON *enabled = compose ? cJSON_GetObjectItem(compose, "postButtonEnabled") : NULL;

    bool ready = false;
    if (draft_len && cJSON_IsNumber(draft_len) && enabled && cJSON_IsBool(enabled)) {
        ready = (draft_len->valuedouble > 0) && cJSON_IsTrue(enabled);
    }

    cJSON_Delete(root);
    return ready;
}

static esp_err_t rpc_get_dom(char *payload, size_t payload_size, bool *ok)
{
    const char *extra = "{\"maxText\":2400,\"maxElements\":80}";
    return ws_server_browser_rpc("get_dom_snapshot", extra, payload, payload_size, ok, 30000);
}

static esp_err_t rpc_execute_action(cJSON *action, char *payload, size_t payload_size, bool *ok)
{
    if (!action || !cJSON_IsObject(action)) return ESP_ERR_INVALID_ARG;
    cJSON *extra = cJSON_CreateObject();
    if (!extra) return ESP_ERR_NO_MEM;
    cJSON_AddItemToObject(extra, "action", cJSON_Duplicate(action, 1));
    char *extra_json = cJSON_PrintUnformatted(extra);
    cJSON_Delete(extra);
    if (!extra_json) return ESP_ERR_NO_MEM;
    esp_err_t err = ws_server_browser_rpc("execute_action", extra_json, payload, payload_size, ok, 30000);
    free(extra_json);
    return err;
}

esp_err_t tool_browser_use_execute(const char *input_json, char *output, size_t output_size)
{
    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(input_json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *command = extract_cmd(root);
    if (!command) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing command/type field");
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(command, "get_dom_snapshot") == 0) {
        cJSON *extra = cJSON_CreateObject();
        cJSON *maxText = cJSON_GetObjectItem(root, "maxText");
        cJSON *maxElements = cJSON_GetObjectItem(root, "maxElements");
        cJSON_AddNumberToObject(extra, "maxText", (maxText && cJSON_IsNumber(maxText)) ? maxText->valuedouble : 3500);
        cJSON_AddNumberToObject(extra, "maxElements", (maxElements && cJSON_IsNumber(maxElements)) ? maxElements->valuedouble : 80);
        char *extra_json = cJSON_PrintUnformatted(extra);
        cJSON_Delete(extra);
        cJSON_Delete(root);
        if (!extra_json) {
            snprintf(output, output_size, "Error: failed to build browser request");
            return ESP_ERR_NO_MEM;
        }

        display_show_agent_status("[BRW]", "Browser Tool", command, true);
        bool ok = false;
        char *rpc_payload = calloc(1, BROWSER_RPC_BUF_SIZE);
        if (!rpc_payload) {
            free(extra_json);
            snprintf(output, output_size, "Error: out of memory");
            return ESP_ERR_NO_MEM;
        }
        esp_err_t err = ws_server_browser_rpc("get_dom_snapshot", extra_json, rpc_payload, BROWSER_RPC_BUF_SIZE, &ok, 30000);
        free(extra_json);
        if (err != ESP_OK) {
            snprintf(output, output_size, "Error: browser rpc failed (%s)", esp_err_to_name(err));
            ESP_LOGW(TAG, "%s", output);
            display_show_agent_status("[BRW]", "Browser Tool", "RPC failed", false);
            free(rpc_payload);
            return err;
        }
        if (!ok) {
            snprintf(output, output_size, "Error: %s", rpc_payload[0] ? rpc_payload : "browser_command_failed");
            display_show_agent_status("[BRW]", "Browser Tool", "Command failed", false);
            free(rpc_payload);
            return ESP_FAIL;
        }
        snprintf(output, output_size, "%s", rpc_payload[0] ? rpc_payload : "{}");
        free(rpc_payload);
        display_show_agent_status("[BRW]", "Browser Tool", "OK", false);
        return ESP_OK;
    } else if (strcmp(command, "execute_action") == 0) {
        cJSON *action = cJSON_GetObjectItem(root, "action");
        if (!action || !cJSON_IsObject(action)) {
            cJSON_Delete(root);
            snprintf(output, output_size, "Error: execute_action requires action object");
            return ESP_ERR_INVALID_ARG;
        }

        cJSON *action_dup = cJSON_Duplicate(action, 1);
        cJSON_Delete(root);
        if (!action_dup) {
            snprintf(output, output_size, "Error: failed to duplicate action");
            return ESP_ERR_NO_MEM;
        }

        cJSON *name = cJSON_GetObjectItem(action_dup, "name");
        const char *action_name = (name && cJSON_IsString(name)) ? name->valuestring : "";

        display_show_agent_status("[BRW]", "Browser Tool", command, true);

        /* Read current page first for Twitter-specific recovery/verification. */
        bool dom_ok = false;
        char *dom_payload = calloc(1, BROWSER_RPC_BUF_SIZE);
        char *rpc_payload = calloc(1, BROWSER_RPC_BUF_SIZE);
        if (!dom_payload || !rpc_payload) {
            cJSON_Delete(action_dup);
            free(dom_payload);
            free(rpc_payload);
            snprintf(output, output_size, "Error: out of memory");
            return ESP_ERR_NO_MEM;
        }
        bool in_twitter = false;
        if (rpc_get_dom(dom_payload, BROWSER_RPC_BUF_SIZE, &dom_ok) == ESP_OK && dom_ok) {
            in_twitter = parse_dom_is_twitter(dom_payload);
        }

        if (in_twitter && strcmp(action_name, "fill") == 0) {
            cJSON *value = cJSON_GetObjectItem(action_dup, "value");
            if (value && cJSON_IsString(value) && value->valuestring) {
                size_t len = strlen(value->valuestring);
                if (len > 200) {
                    char trunc[201] = {0};
                    memcpy(trunc, value->valuestring, 200);
                    cJSON_ReplaceItemInObject(action_dup, "value", cJSON_CreateString(trunc));
                }
            }
        }

        bool ok = false;
        esp_err_t err = rpc_execute_action(action_dup, rpc_payload, BROWSER_RPC_BUF_SIZE, &ok);

        /* Recovery 1: open composer directly if click target not found on Twitter. */
        if ((err == ESP_OK) && !ok && strcmp(action_name, "click") == 0 && in_twitter &&
            str_contains(rpc_payload, "click target not found")) {
            cJSON *nav = cJSON_CreateObject();
            cJSON_AddStringToObject(nav, "name", "navigate");
            cJSON_AddStringToObject(nav, "url", "https://x.com/compose/post");
            err = rpc_execute_action(nav, rpc_payload, BROWSER_RPC_BUF_SIZE, &ok);
            cJSON_Delete(nav);
        }

        /* Recovery 2: ensure fill actually enabled Post button on Twitter. */
        if (err == ESP_OK && ok && strcmp(action_name, "fill") == 0 && in_twitter) {
            bool ready = false;
            for (int i = 0; i < 5; i++) {
                bool check_ok = false;
                dom_payload[0] = '\0';
                if (rpc_get_dom(dom_payload, BROWSER_RPC_BUF_SIZE, &check_ok) == ESP_OK && check_ok) {
                    ready = parse_twitter_fill_ready(dom_payload);
                }
                if (ready) break;
                err = rpc_execute_action(action_dup, rpc_payload, BROWSER_RPC_BUF_SIZE, &ok);
                if (err != ESP_OK || !ok) break;
            }
            if (!ready && err == ESP_OK && ok) {
                ok = false;
                snprintf(rpc_payload, BROWSER_RPC_BUF_SIZE, "Twitter fill not applied to editor state (post button still disabled).");
            }
        }

        cJSON_Delete(action_dup);

        if (err != ESP_OK) {
            snprintf(output, output_size, "Error: browser rpc failed (%s)", esp_err_to_name(err));
            ESP_LOGW(TAG, "%s", output);
            display_show_agent_status("[BRW]", "Browser Tool", "RPC failed", false);
            free(dom_payload);
            free(rpc_payload);
            return err;
        }
        if (!ok) {
            snprintf(output, output_size, "Error: %s", rpc_payload[0] ? rpc_payload : "browser_command_failed");
            display_show_agent_status("[BRW]", "Browser Tool", "Command failed", false);
            free(dom_payload);
            free(rpc_payload);
            return ESP_FAIL;
        }
        snprintf(output, output_size, "%s", rpc_payload[0] ? rpc_payload : "{}");
        free(dom_payload);
        free(rpc_payload);
        display_show_agent_status("[BRW]", "Browser Tool", "OK", false);
        return ESP_OK;
    } else {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: unsupported command '%s'", command);
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_FAIL;
}
