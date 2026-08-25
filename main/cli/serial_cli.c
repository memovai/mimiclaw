#include "serial_cli.h"
#include "mimi_config.h"
#include "wifi/wifi_manager.h"
#include "channels/telegram/telegram_bot.h"
#include "channels/feishu/feishu_bot.h"
#include "llm/llm_proxy.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "proxy/http_proxy.h"
#include "tools/tool_registry.h"
#include "tools/tool_web_search.h"
#include "cron/cron_service.h"
#include "heartbeat/heartbeat.h"
#include "skills/skill_loader.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_console.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "cli";

/* --- wifi_set command --- */
static struct {
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_end *end;
} wifi_set_args;

static int cmd_wifi_set(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&wifi_set_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, wifi_set_args.end, argv[0]);
        return 1;
    }
    wifi_manager_set_credentials(wifi_set_args.ssid->sval[0],
                                  wifi_set_args.password->sval[0]);
    printf("WiFi credentials saved. Restart to apply.\n");
    return 0;
}

/* --- wifi_status command --- */
static int cmd_wifi_status(int argc, char **argv)
{
    printf("WiFi connected: %s\n", wifi_manager_is_connected() ? "yes" : "no");
    printf("IP: %s\n", wifi_manager_get_ip());
    return 0;
}

/* --- set_tg_token command --- */
static struct {
    struct arg_str *token;
    struct arg_end *end;
} tg_token_args;

/* --- tg_send command --- */
static struct {
    struct arg_str *chat_id;
    struct arg_str *text;
    struct arg_end *end;
} tg_send_args;

static int cmd_set_tg_token(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tg_token_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tg_token_args.end, argv[0]);
        return 1;
    }
    telegram_set_token(tg_token_args.token->sval[0]);
    printf("Telegram bot token saved.\n");
    return 0;
}

static int cmd_tg_send(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tg_send_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tg_send_args.end, argv[0]);
        return 1;
    }

    esp_err_t err = telegram_send_message(tg_send_args.chat_id->sval[0],
                                         tg_send_args.text->sval[0]);
    printf("tg_send status: %s\n", esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

/* --- set_feishu_creds command --- */
static struct {
    struct arg_str *app_id;
    struct arg_str *app_secret;
    struct arg_end *end;
} feishu_creds_args;

/* --- feishu_send command --- */
static struct {
    struct arg_str *receive_id;
    struct arg_str *text;
    struct arg_end *end;
} feishu_send_args;

static int cmd_set_feishu_creds(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&feishu_creds_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, feishu_creds_args.end, argv[0]);
        return 1;
    }
    feishu_set_credentials(feishu_creds_args.app_id->sval[0],
                          feishu_creds_args.app_secret->sval[0]);
    printf("Feishu credentials saved.\n");
    return 0;
}

static int cmd_feishu_send(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&feishu_send_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, feishu_send_args.end, argv[0]);
        return 1;
    }

    esp_err_t err = feishu_send_message(feishu_send_args.receive_id->sval[0],
                                        feishu_send_args.text->sval[0]);
    printf("feishu_send status: %s\n", esp_err_to_name(err));
    return (err == ESP_OK) ? 0 : 1;
}

/* --- set_api_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} api_key_args;

static int cmd_set_api_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&api_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, api_key_args.end, argv[0]);
        return 1;
    }
    llm_set_api_key(api_key_args.key->sval[0]);
    printf("API key saved.\n");
    return 0;
}

/* --- set_model command --- */
static struct {
    struct arg_str *model;
    struct arg_end *end;
} model_args;

static int cmd_set_model(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&model_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, model_args.end, argv[0]);
        return 1;
    }
    llm_set_model(model_args.model->sval[0]);
    printf("Model set.\n");
    return 0;
}

/* --- set_model_provider command --- */
static struct {
    struct arg_str *provider;
    struct arg_end *end;
} provider_args;

static int cmd_set_model_provider(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&provider_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, provider_args.end, argv[0]);
        return 1;
    }
    llm_set_provider(provider_args.provider->sval[0]);
    printf("Model provider set.\n");
    return 0;
}

/* --- memory_read command --- */
static int cmd_memory_read(int argc, char **argv)
{
    char *buf = malloc(4096);
    if (!buf) {
        printf("Out of memory.\n");
        return 1;
    }
    if (memory_read_long_term(buf, 4096) == ESP_OK && buf[0]) {
        printf("=== MEMORY.md ===\n%s\n=================\n", buf);
    } else {
        printf("MEMORY.md is empty or not found.\n");
    }
    free(buf);
    return 0;
}

/* --- memory_write command --- */
static struct {
    struct arg_str *content;
    struct arg_end *end;
} memory_write_args;

static int cmd_memory_write(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&memory_write_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, memory_write_args.end, argv[0]);
        return 1;
    }
    memory_write_long_term(memory_write_args.content->sval[0]);
    printf("MEMORY.md updated.\n");
    return 0;
}

/* --- session_list command --- */
static int cmd_session_list(int argc, char **argv)
{
    printf("Sessions:\n");
    session_list();
    return 0;
}

/* --- session_clear command --- */
static struct {
    struct arg_str *chat_id;
    struct arg_end *end;
} session_clear_args;

static int cmd_session_clear(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&session_clear_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, session_clear_args.end, argv[0]);
        return 1;
    }
    if (session_clear(session_clear_args.chat_id->sval[0]) == ESP_OK) {
        printf("Session cleared.\n");
    } else {
        printf("Session not found.\n");
    }
    return 0;
}

/* --- heap_info command --- */
static int cmd_heap_info(int argc, char **argv)
{
    printf("Internal free: %d bytes\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("PSRAM free:    %d bytes\n",
           (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("Total free:    %d bytes\n",
           (int)esp_get_free_heap_size());
    return 0;
}

/* --- set_proxy command --- */
static struct {
    struct arg_str *host;
    struct arg_int *port;
    struct arg_str *type;
    struct arg_end *end;
} proxy_args;

static int cmd_set_proxy(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&proxy_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, proxy_args.end, argv[0]);
        return 1;
    }
    const char *proxy_type = "http";
    if (proxy_args.type->count > 0 && proxy_args.type->sval[0] && proxy_args.type->sval[0][0]) {
        proxy_type = proxy_args.type->sval[0];
    }
    if (strcmp(proxy_type, "http") != 0 && strcmp(proxy_type, "socks5") != 0) {
        printf("Invalid proxy type: %s. Use http or socks5.\n", proxy_type);
        return 1;
    }

    http_proxy_set(proxy_args.host->sval[0], (uint16_t)proxy_args.port->ival[0], proxy_type);
    printf("Proxy set. Restart to apply.\n");
    return 0;
}

/* --- clear_proxy command --- */
static int cmd_clear_proxy(int argc, char **argv)
{
    http_proxy_clear();
    printf("Proxy cleared. Restart to apply.\n");
    return 0;
}

/* --- set_search_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} search_key_args;

static int cmd_set_search_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&search_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, search_key_args.end, argv[0]);
        return 1;
    }
    tool_web_search_set_key(search_key_args.key->sval[0]);
    printf("Search API key saved.\n");
    return 0;
}

/* --- set_tavily_key command --- */
static struct {
    struct arg_str *key;
    struct arg_end *end;
} tavily_key_args;

static int cmd_set_tavily_key(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tavily_key_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, tavily_key_args.end, argv[0]);
        return 1;
    }
    tool_web_search_set_tavily_key(tavily_key_args.key->sval[0]);
    printf("Tavily API key saved.\n");
    return 0;
}

/* --- wifi_scan command --- */
static int cmd_wifi_scan(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    wifi_manager_scan_and_print();
    return 0;
}

/* --- skill_list command --- */
static int cmd_skill_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    char *buf = malloc(4096);
    if (!buf) {
        printf("Out of memory.\n");
        return 1;
    }

    size_t n = skill_loader_build_summary(buf, 4096);
    if (n == 0) {
        printf("No skills found under " MIMI_SKILLS_PREFIX ".\n");
    } else {
        printf("=== Skills ===\n%s", buf);
    }
    free(buf);
    return 0;
}

/* --- skill_show command --- */
static struct {
    struct arg_str *name;
    struct arg_end *end;
} skill_show_args;

static bool has_md_suffix(const char *name)
{
    size_t len = strlen(name);
    return (len >= 3) && strcmp(name + len - 3, ".md") == 0;
}

static bool build_skill_path(const char *name, char *out, size_t out_size)
{
    if (!name || !name[0]) return false;
    if (strstr(name, "..") != NULL) return false;
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) return false;

    if (has_md_suffix(name)) {
        snprintf(out, out_size, MIMI_SKILLS_PREFIX "%s", name);
    } else {
        snprintf(out, out_size, MIMI_SKILLS_PREFIX "%s.md", name);
    }
    return true;
}

static int cmd_skill_show(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&skill_show_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, skill_show_args.end, argv[0]);
        return 1;
    }

    char path[128];
    if (!build_skill_path(skill_show_args.name->sval[0], path, sizeof(path))) {
        printf("Invalid skill name.\n");
        return 1;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        printf("Skill not found: %s\n", path);
        return 1;
    }

    printf("=== %s ===\n", path);
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        fputs(line, stdout);
    }
    fclose(f);
    printf("\n============\n");
    return 0;
}

/* --- skill_search command --- */
static struct {
    struct arg_str *keyword;
    struct arg_end *end;
} skill_search_args;

static bool contains_nocase(const char *text, const char *keyword)
{
    if (!text || !keyword || !keyword[0]) return false;

    size_t key_len = strlen(keyword);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < key_len && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)keyword[i])) {
            i++;
        }
        if (i == key_len) return true;
    }
    return false;
}

static int cmd_skill_search(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&skill_search_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, skill_search_args.end, argv[0]);
        return 1;
    }

    const char *keyword = skill_search_args.keyword->sval[0];
    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    if (!dir) {
        printf("Cannot open " MIMI_SPIFFS_BASE ".\n");
        return 1;
    }

    const char *prefix = "skills/";
    const size_t prefix_len = strlen(prefix);
    int matches = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        size_t name_len = strlen(name);

        if (strncmp(name, prefix, prefix_len) != 0) continue;
        if (name_len < prefix_len + 4) continue;
        if (strcmp(name + name_len - 3, ".md") != 0) continue;

        char full_path[296];
        snprintf(full_path, sizeof(full_path), MIMI_SPIFFS_BASE "/%s", name);

        bool file_matched = contains_nocase(name, keyword);
        int matched_line = 0;

        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        char line[256];
        int line_no = 0;
        while (!file_matched && fgets(line, sizeof(line), f)) {
            line_no++;
            if (contains_nocase(line, keyword)) {
                file_matched = true;
                matched_line = line_no;
            }
        }
        fclose(f);

        if (file_matched) {
            matches++;
            if (matched_line > 0) {
                printf("- %s (matched at line %d)\n", full_path, matched_line);
            } else {
                printf("- %s (matched in filename)\n", full_path);
            }
        }
    }

    closedir(dir);
    if (matches == 0) {
        printf("No skills matched keyword: %s\n", keyword);
    } else {
        printf("Total matches: %d\n", matches);
    }
    return 0;
}

/* --- config_show command --- */
static void print_config(const char *label, const char *ns, const char *key,
                         const char *build_val, bool mask)
{
    char nvs_val[128] = {0};
    const char *source = "not set";
    const char *display = "(empty)";

    /* NVS takes highest priority */
    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(nvs_val);
        if (nvs_get_str(nvs, key, nvs_val, &len) == ESP_OK && nvs_val[0]) {
            source = "NVS";
            display = nvs_val;
        }
        nvs_close(nvs);
    }

    /* Fall back to build-time value */
    if (strcmp(source, "not set") == 0 && build_val[0] != '\0') {
        source = "build";
        display = build_val;
    }

    if (mask && strlen(display) > 6 && strcmp(display, "(empty)") != 0) {
        printf("  %-14s: %.4s****  [%s]\n", label, display, source);
    } else {
        printf("  %-14s: %s  [%s]\n", label, display, source);
    }
}

static void print_config_u16(const char *label, const char *ns, const char *key,
                             const char *build_val)
{
    char nvs_val[16] = {0};
    const char *source = "not set";
    const char *display = "(empty)";

    nvs_handle_t nvs;
    if (nvs_open(ns, NVS_READONLY, &nvs) == ESP_OK) {
        uint16_t value = 0;
        if (nvs_get_u16(nvs, key, &value) == ESP_OK && value > 0) {
            snprintf(nvs_val, sizeof(nvs_val), "%u", (unsigned)value);
            source = "NVS";
            display = nvs_val;
        }
        nvs_close(nvs);
    }

    if (strcmp(source, "not set") == 0 && build_val[0] != '\0') {
        source = "build";
        display = build_val;
    }

    printf("  %-14s: %s  [%s]\n", label, display, source);
}

static int cmd_config_show(int argc, char **argv)
{
    printf("=== Current Configuration ===\n");
    print_config("WiFi SSID",  MIMI_NVS_WIFI,   MIMI_NVS_KEY_SSID,     MIMI_SECRET_WIFI_SSID,  false);
    print_config("WiFi Pass",  MIMI_NVS_WIFI,   MIMI_NVS_KEY_PASS,     MIMI_SECRET_WIFI_PASS,  true);
    print_config("TG Token",   MIMI_NVS_TG,     MIMI_NVS_KEY_TG_TOKEN, MIMI_SECRET_TG_TOKEN,   true);
    print_config("API Key",    MIMI_NVS_LLM,    MIMI_NVS_KEY_API_KEY,  MIMI_SECRET_API_KEY,    true);
    print_config("Model",      MIMI_NVS_LLM,    MIMI_NVS_KEY_MODEL,    MIMI_SECRET_MODEL,      false);
    print_config("Provider",   MIMI_NVS_LLM,    MIMI_NVS_KEY_PROVIDER, MIMI_SECRET_MODEL_PROVIDER, false);
    print_config("Proxy Host", MIMI_NVS_PROXY,  MIMI_NVS_KEY_PROXY_HOST, MIMI_SECRET_PROXY_HOST, false);
    print_config_u16("Proxy Port", MIMI_NVS_PROXY, MIMI_NVS_KEY_PROXY_PORT, MIMI_SECRET_PROXY_PORT);
    print_config("Search Key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_API_KEY,  MIMI_SECRET_SEARCH_KEY, true);
    print_config("Tavily Key", MIMI_NVS_SEARCH, MIMI_NVS_KEY_TAVILY_KEY, MIMI_SECRET_TAVILY_KEY, true);
    printf("=============================\n");
    return 0;
}

/* --- config_reset command --- */
static int cmd_config_reset(int argc, char **argv)
{
    const char *namespaces[] = {
        MIMI_NVS_WIFI, MIMI_NVS_TG, MIMI_NVS_LLM, MIMI_NVS_PROXY, MIMI_NVS_SEARCH
    };
    for (int i = 0; i < 5; i++) {
        nvs_handle_t nvs;
        if (nvs_open(namespaces[i], NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_all(nvs);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
    printf("All NVS config cleared. Build-time defaults will be used on restart.\n");
    return 0;
}

/* --- heartbeat_trigger command --- */
static int cmd_heartbeat_trigger(int argc, char **argv)
{
    printf("Checking HEARTBEAT.md...\n");
    if (heartbeat_trigger()) {
        printf("Heartbeat: agent prompted with pending tasks.\n");
    } else {
        printf("Heartbeat: no actionable tasks found.\n");
    }
    return 0;
}

/* --- cron_start command --- */
static int cmd_cron_start(int argc, char **argv)
{
    esp_err_t err = cron_service_start();
    if (err == ESP_OK) {
        printf("Cron service started.\n");
        return 0;
    }

    printf("Failed to start cron service: %s\n", esp_err_to_name(err));
    return 1;
}

static int cmd_tool_exec(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: tool_exec <name> [json]\n");
        return 1;
    }

    const char *tool_name = argv[1];
    const char *input_json = (argc >= 3) ? argv[2] : "{}";

    char *output = calloc(1, 4096);
    if (!output) {
        printf("Out of memory.\n");
        return 1;
    }

    esp_err_t err = tool_registry_execute(tool_name, input_json, output, 4096);
    printf("tool_exec status: %s\n", esp_err_to_name(err));
    printf("%s\n", output[0] ? output : "(empty)");
    free(output);
    return (err == ESP_OK) ? 0 : 1;
}

/* --- web_search command --- */
static struct {
    struct arg_str *query;
    struct arg_end *end;
} web_search_args;

typedef struct {
    const char *input_json;
    char *output;
    size_t output_size;
    esp_err_t err;
    SemaphoreHandle_t done;
} web_search_task_ctx_t;

static void web_search_task(void *arg)
{
    web_search_task_ctx_t *task_ctx = (web_search_task_ctx_t *)arg;
    task_ctx->err = tool_web_search_execute(task_ctx->input_json, task_ctx->output, task_ctx->output_size);
    xSemaphoreGive(task_ctx->done);
    vTaskDelete(NULL);
}

static bool json_escape_string(const char *in, char *out, size_t out_size)
{
    if (!in || !out || out_size == 0) return false;
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0'; ++i) {
        const char c = in[i];
        const char *esc = NULL;
        switch (c) {
            case '\\': esc = "\\\\"; break;
            case '\"': esc = "\\\""; break;
            case '\n': esc = "\\n"; break;
            case '\r': esc = "\\r"; break;
            case '\t': esc = "\\t"; break;
            default: break;
        }
        if (esc) {
            size_t n = strlen(esc);
            if (o + n >= out_size) return false;
            memcpy(&out[o], esc, n);
            o += n;
            continue;
        }
        if ((unsigned char)c < 0x20) {
            continue;
        }
        if (o + 1 >= out_size) return false;
        out[o++] = c;
    }
    out[o] = '\0';
    return true;
}

static int cmd_web_search(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&web_search_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, web_search_args.end, argv[0]);
        return 1;
    }

    char escaped_query[512];
    if (!json_escape_string(web_search_args.query->sval[0], escaped_query, sizeof(escaped_query))) {
        printf("Query too long.\n");
        return 1;
    }

    char input_json[640];
    int n = snprintf(input_json, sizeof(input_json), "{\"query\":\"%s\"}", escaped_query);
    if (n <= 0 || n >= (int)sizeof(input_json)) {
        printf("Query too long.\n");
        return 1;
    }

    char *output = calloc(1, 4096);
    if (!output) {
        printf("Out of memory.\n");
        return 1;
    }

    web_search_task_ctx_t *ctx = calloc(1, sizeof(*ctx));
    char *input_copy = strdup(input_json);
    if (!ctx || !input_copy) {
        free(input_copy);
        free(ctx);
        free(output);
        printf("Out of memory.\n");
        return 1;
    }

    ctx->input_json = input_copy;
    ctx->output = output;
    ctx->output_size = 4096;
    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) {
        free(input_copy);
        free(ctx);
        free(output);
        printf("Out of memory.\n");
        return 1;
    }

    if (xTaskCreate(web_search_task, "cli_web_search", 20 * 1024, ctx, 5, NULL) != pdPASS) {
        vSemaphoreDelete(ctx->done);
        free(input_copy);
        free(ctx);
        free(output);
        printf("Failed to start web_search task.\n");
        return 1;
    }

    if (xSemaphoreTake(ctx->done, pdMS_TO_TICKS(45000)) != pdTRUE) {
        printf("web_search status: timeout\n");
        vSemaphoreDelete(ctx->done);
        free(input_copy);
        free(ctx);
        free(output);
        return 1;
    }
    esp_err_t err = ctx->err;
    vSemaphoreDelete(ctx->done);
    free(input_copy);
    free(ctx);

    printf("web_search status: %s\n", esp_err_to_name(err));
    printf("%s\n", output[0] ? output : "(empty)");
    free(output);
    return (err == ESP_OK) ? 0 : 1;
}

/* --- restart command --- */
static int cmd_restart(int argc, char **argv)
{
    printf("Restarting...\n");
    esp_restart();
    return 0;  /* unreachable */
}

esp_err_t serial_cli_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "mimi> ";
    repl_config.max_cmdline_length = 256;

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));
#else
    ESP_LOGE(TAG, "No supported console backend is enabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif

    /* Register commands */
    esp_console_register_help_command();

    /* set_wifi */
    wifi_set_args.ssid = arg_str1(NULL, NULL, "<ssid>", "WiFi SSID");
    wifi_set_args.password = arg_str1(NULL, NULL, "<password>", "WiFi password");
    wifi_set_args.end = arg_end(2);
    esp_console_cmd_t wifi_set_cmd = {
        .command = "set_wifi",
        .help = "Set WiFi SSID and password (e.g. set_wifi MySSID MyPass)",
        .func = &cmd_wifi_set,
        .argtable = &wifi_set_args,
    };
    esp_console_cmd_register(&wifi_set_cmd);

    /* wifi_status */
    esp_console_cmd_t wifi_status_cmd = {
        .command = "wifi_status",
        .help = "Show WiFi connection status",
        .func = &cmd_wifi_status,
    };
    esp_console_cmd_register(&wifi_status_cmd);

    /* wifi_scan */
    esp_console_cmd_t wifi_scan_cmd = {
        .command = "wifi_scan",
        .help = "Scan and list nearby WiFi APs",
        .func = &cmd_wifi_scan,
    };
    esp_console_cmd_register(&wifi_scan_cmd);

    /* set_tg_token */
    tg_token_args.token = arg_str1(NULL, NULL, "<token>", "Telegram bot token");
    tg_token_args.end = arg_end(1);
    esp_console_cmd_t tg_token_cmd = {
        .command = "set_tg_token",
        .help = "Set Telegram bot token",
        .func = &cmd_set_tg_token,
        .argtable = &tg_token_args,
    };
    esp_console_cmd_register(&tg_token_cmd);

    /* tg_send */
    tg_send_args.chat_id = arg_str1(NULL, NULL, "<chat_id>", "Telegram chat ID");
    tg_send_args.text = arg_str1(NULL, NULL, "<text>", "Text message (quote if contains spaces)");
    tg_send_args.end = arg_end(2);

    esp_console_cmd_t tg_send_cmd = {
        .command = "tg_send",
        .help = "Send Telegram text: tg_send <chat_id> \"hello\"",
        .func = &cmd_tg_send,
        .argtable = &tg_send_args,
    };
    esp_console_cmd_register(&tg_send_cmd);


    /* set_feishu_creds */
    feishu_creds_args.app_id = arg_str1(NULL, NULL, "<app_id>", "Feishu App ID");
    feishu_creds_args.app_secret = arg_str1(NULL, NULL, "<app_secret>", "Feishu App Secret");
    feishu_creds_args.end = arg_end(2);
    esp_console_cmd_t feishu_creds_cmd = {
        .command = "set_feishu_creds",
        .help = "Set Feishu app credentials (app_id app_secret)",
        .func = &cmd_set_feishu_creds,
        .argtable = &feishu_creds_args,
    };
    esp_console_cmd_register(&feishu_creds_cmd);

    /* feishu_send */
    feishu_send_args.receive_id = arg_str1(NULL, NULL, "<receive_id>", "Feishu open_id/chat_id");
    feishu_send_args.text = arg_str1(NULL, NULL, "<text>", "Text message (quote if contains spaces)");
    feishu_send_args.end = arg_end(2);
    esp_console_cmd_t feishu_send_cmd = {
        .command = "feishu_send",
        .help = "Send Feishu text: feishu_send <open_id|chat_id> \"hello\"",
        .func = &cmd_feishu_send,
        .argtable = &feishu_send_args,
    };
    esp_console_cmd_register(&feishu_send_cmd);

    /* set_api_key */
    api_key_args.key = arg_str1(NULL, NULL, "<key>", "LLM API key");
    api_key_args.end = arg_end(1);
    esp_console_cmd_t api_key_cmd = {
        .command = "set_api_key",
        .help = "Set LLM API key",
        .func = &cmd_set_api_key,
        .argtable = &api_key_args,
    };
    esp_console_cmd_register(&api_key_cmd);

    /* set_model */
    model_args.model = arg_str1(NULL, NULL, "<model>", "Model identifier");
    model_args.end = arg_end(1);
    esp_console_cmd_t model_cmd = {
        .command = "set_model",
        .help = "Set LLM model (default: " MIMI_LLM_DEFAULT_MODEL ")",
        .func = &cmd_set_model,
        .argtable = &model_args,
    };
    esp_console_cmd_register(&model_cmd);

    /* set_model_provider */
    provider_args.provider = arg_str1(NULL, NULL, "<provider>", "Model provider (anthropic|openai)");
    provider_args.end = arg_end(1);
    esp_console_cmd_t provider_cmd = {
        .command = "set_model_provider",
        .help = "Set LLM model provider (default: " MIMI_LLM_PROVIDER_DEFAULT ")",
        .func = &cmd_set_model_provider,
        .argtable = &provider_args,
    };
    esp_console_cmd_register(&provider_cmd);

    /* skill_list */
    esp_console_cmd_t skill_list_cmd = {
        .command = "skill_list",
        .help = "List installed skills from " MIMI_SKILLS_PREFIX,
        .func = &cmd_skill_list,
    };
    esp_console_cmd_register(&skill_list_cmd);

    /* skill_show */
    skill_show_args.name = arg_str1(NULL, NULL, "<name>", "Skill name (e.g. weather or weather.md)");
    skill_show_args.end = arg_end(1);
    esp_console_cmd_t skill_show_cmd = {
        .command = "skill_show",
        .help = "Print full content of one skill file",
        .func = &cmd_skill_show,
        .argtable = &skill_show_args,
    };
    esp_console_cmd_register(&skill_show_cmd);

    /* skill_search */
    skill_search_args.keyword = arg_str1(NULL, NULL, "<keyword>", "Keyword to search in skills");
    skill_search_args.end = arg_end(1);
    esp_console_cmd_t skill_search_cmd = {
        .command = "skill_search",
        .help = "Search skill files by keyword (filename + content)",
        .func = &cmd_skill_search,
        .argtable = &skill_search_args,
    };
    esp_console_cmd_register(&skill_search_cmd);

    /* memory_read */
    esp_console_cmd_t mem_read_cmd = {
        .command = "memory_read",
        .help = "Read MEMORY.md",
        .func = &cmd_memory_read,
    };
    esp_console_cmd_register(&mem_read_cmd);

    /* memory_write */
    memory_write_args.content = arg_str1(NULL, NULL, "<content>", "Content to write");
    memory_write_args.end = arg_end(1);
    esp_console_cmd_t mem_write_cmd = {
        .command = "memory_write",
        .help = "Write to MEMORY.md",
        .func = &cmd_memory_write,
        .argtable = &memory_write_args,
    };
    esp_console_cmd_register(&mem_write_cmd);

    /* session_list */
    esp_console_cmd_t sess_list_cmd = {
        .command = "session_list",
        .help = "List all sessions",
        .func = &cmd_session_list,
    };
    esp_console_cmd_register(&sess_list_cmd);

    /* session_clear */
    session_clear_args.chat_id = arg_str1(NULL, NULL, "<chat_id>", "Chat ID to clear");
    session_clear_args.end = arg_end(1);
    esp_console_cmd_t sess_clear_cmd = {
        .command = "session_clear",
        .help = "Clear a session",
        .func = &cmd_session_clear,
        .argtable = &session_clear_args,
    };
    esp_console_cmd_register(&sess_clear_cmd);

    /* heap_info */
    esp_console_cmd_t heap_cmd = {
        .command = "heap_info",
        .help = "Show heap memory usage",
        .func = &cmd_heap_info,
    };
    esp_console_cmd_register(&heap_cmd);

    /* set_search_key */
    search_key_args.key = arg_str1(NULL, NULL, "<key>", "Brave Search API key");
    search_key_args.end = arg_end(1);
    esp_console_cmd_t search_key_cmd = {
        .command = "set_search_key",
        .help = "Set Brave Search API key for web_search tool",
        .func = &cmd_set_search_key,
        .argtable = &search_key_args,
    };
    esp_console_cmd_register(&search_key_cmd);

    /* set_tavily_key */
    tavily_key_args.key = arg_str1(NULL, NULL, "<key>", "Tavily Search API key");
    tavily_key_args.end = arg_end(1);
    esp_console_cmd_t tavily_key_cmd = {
        .command = "set_tavily_key",
        .help = "Set Tavily API key for web_search tool",
        .func = &cmd_set_tavily_key,
        .argtable = &tavily_key_args,
    };
    esp_console_cmd_register(&tavily_key_cmd);

    /* set_proxy */
    proxy_args.host = arg_str1(NULL, NULL, "<host>", "Proxy host/IP");
    proxy_args.port = arg_int1(NULL, NULL, "<port>", "Proxy port");
    proxy_args.type = arg_str0(NULL, NULL, "<type>", "Proxy type: http|socks5 (default: http)");
    proxy_args.end = arg_end(3);
    esp_console_cmd_t proxy_cmd = {
        .command = "set_proxy",
        .help = "Set proxy (e.g. set_proxy 192.168.1.83 7897 [http|socks5])",
        .func = &cmd_set_proxy,
        .argtable = &proxy_args,
    };
    esp_console_cmd_register(&proxy_cmd);

    /* clear_proxy */
    esp_console_cmd_t clear_proxy_cmd = {
        .command = "clear_proxy",
        .help = "Remove proxy configuration",
        .func = &cmd_clear_proxy,
    };
    esp_console_cmd_register(&clear_proxy_cmd);

    /* config_show */
    esp_console_cmd_t config_show_cmd = {
        .command = "config_show",
        .help = "Show current configuration (build-time + NVS)",
        .func = &cmd_config_show,
    };
    esp_console_cmd_register(&config_show_cmd);

    /* config_reset */
    esp_console_cmd_t config_reset_cmd = {
        .command = "config_reset",
        .help = "Clear all NVS overrides, revert to build-time defaults",
        .func = &cmd_config_reset,
    };
    esp_console_cmd_register(&config_reset_cmd);

    /* heartbeat_trigger */
    esp_console_cmd_t heartbeat_cmd = {
        .command = "heartbeat_trigger",
        .help = "Manually trigger a heartbeat check",
        .func = &cmd_heartbeat_trigger,
    };
    esp_console_cmd_register(&heartbeat_cmd);

    /* cron_start */
    esp_console_cmd_t cron_start_cmd = {
        .command = "cron_start",
        .help = "Start cron scheduler timer now",
        .func = &cmd_cron_start,
    };
    esp_console_cmd_register(&cron_start_cmd);

    /* tool_exec */
    esp_console_cmd_t tool_exec_cmd = {
        .command = "tool_exec",
        .help = "Execute a registered tool: tool_exec <name> '{...json...}'",
        .func = &cmd_tool_exec,
    };
    esp_console_cmd_register(&tool_exec_cmd);

    /* web_search */
    web_search_args.query = arg_str1(NULL, NULL, "<query>", "Search query");
    web_search_args.end = arg_end(1);
    esp_console_cmd_t web_search_cmd = {
        .command = "web_search",
        .help = "Run web search tool directly (e.g. web_search \"latest esp-idf\")",
        .func = &cmd_web_search,
        .argtable = &web_search_args,
    };
    esp_console_cmd_register(&web_search_cmd);

    /* restart */
    esp_console_cmd_t restart_cmd = {
        .command = "restart",
        .help = "Restart the device",
        .func = &cmd_restart,
    };
    esp_console_cmd_register(&restart_cmd);

    /* Start REPL */
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "Serial CLI started");

    return ESP_OK;
}
