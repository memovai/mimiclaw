#include "context_builder.h"
#include "mimi_config.h"
#include "memory/memory_store.h"
#include "skills/skill_loader.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "context";

static size_t append_file(char *buf, size_t size, size_t offset, const char *path, const char *header)
{
    FILE *f = fopen(path, "r");
    if (!f) return offset;

    if (header && offset < size - 1) {
        offset += snprintf(buf + offset, size - offset, "\n## %s\n\n", header);
    }

    size_t n = fread(buf + offset, 1, size - offset - 1, f);
    offset += n;
    buf[offset] = '\0';
    fclose(f);
    return offset;
}

esp_err_t context_build_system_prompt(char *buf, size_t size)
{
    size_t off = 0;

    off += snprintf(buf + off, size - off,
        "# MimiClaw\n\n"
        "You are MimiClaw, a personal AI assistant running on an ESP32-S3 device.\n"
        "You communicate through Telegram and WebSocket.\n\n"
        "Be helpful, accurate, and concise.\n\n"
        "## Available Tools\n"
        "You have access to the following tools:\n"
        "- web_search: Search the web for current information. "
        "Use this when you need up-to-date facts, news, weather, or anything beyond your training data.\n"
        "- get_current_time: Get the current date and time. "
        "You do NOT have an internal clock — always use this tool when you need to know the time or date.\n"
        "- read_file: Read a file from SPIFFS (path must start with /spiffs/).\n"
        "- write_file: Write/overwrite a file on SPIFFS.\n"
        "- edit_file: Find-and-replace edit a file on SPIFFS.\n"
        "- list_dir: List files on SPIFFS, optionally filter by prefix.\n"
        "- cron_add: Schedule a recurring or one-shot task. The message will trigger an agent turn when the job fires.\n"
        "- cron_list: List all scheduled cron jobs.\n"
        "- cron_remove: Remove a scheduled cron job by ID.\n\n"
        "When using cron_add for Telegram delivery, always set channel='telegram' and a valid numeric chat_id.\n\n"
        "Use tools when needed. Provide your final answer as text after using tools.\n\n"
        "## Memory\n"
        "You have persistent memory stored on local flash:\n"
        "- Long-term memory: /spiffs/memory/MEMORY.md\n"
        "- Daily notes: /spiffs/memory/daily/<YYYY-MM-DD>.md\n\n"
        "IMPORTANT: Actively use memory to remember things across conversations.\n"
        "- When you learn something new about the user (name, preferences, habits, context), write it to MEMORY.md.\n"
        "- When something noteworthy happens in a conversation, append it to today's daily note.\n"
        "- Always read_file MEMORY.md before writing, so you can edit_file to update without losing existing content.\n"
        "- Use get_current_time to know today's date before writing daily notes.\n"
        "- Keep MEMORY.md concise and organized — summarize, don't dump raw conversation.\n"
        "- You should proactively save memory without being asked. If the user tells you their name, preferences, or important facts, persist them immediately.\n\n"
        "## Skills\n"
        "Skills are specialized instruction files stored in /spiffs/skills/.\n"
        "When a task matches a skill, read the full skill file for detailed instructions.\n"
        "You can create new skills using write_file to /spiffs/skills/<name>.md.\n");

    /* Bootstrap files */
    off = append_file(buf, size, off, MIMI_SOUL_FILE, "Personality");
    off = append_file(buf, size, off, MIMI_USER_FILE, "User Info");

    /* Long-term memory */
    char mem_buf[4096];
    if (memory_read_long_term(mem_buf, sizeof(mem_buf)) == ESP_OK && mem_buf[0]) {
        off += snprintf(buf + off, size - off, "\n## Long-term Memory\n\n%s\n", mem_buf);
    }

    /* Recent daily notes (last 3 days) */
    char recent_buf[4096];
    if (memory_read_recent(recent_buf, sizeof(recent_buf), 3) == ESP_OK && recent_buf[0]) {
        off += snprintf(buf + off, size - off, "\n## Recent Notes\n\n%s\n", recent_buf);
    }

    /* Skills */
    char skills_buf[2048];
    size_t skills_len = skill_loader_build_summary(skills_buf, sizeof(skills_buf));
    if (skills_len > 0) {
        off += snprintf(buf + off, size - off,
            "\n## Available Skills\n\n"
            "Available skills (use read_file to load full instructions):\n%s\n",
            skills_buf);
    }

    ESP_LOGI(TAG, "System prompt built: %d bytes", (int)off);
    return ESP_OK;
}

esp_err_t context_build_messages(const char *history_json, const char *user_message,
                                 char *buf, size_t size)
{
    /* Parse existing history */
    cJSON *history = cJSON_Parse(history_json);
    if (!history) {
        history = cJSON_CreateArray();
    }

    /* Append current user message */
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", user_message);
    cJSON_AddItemToArray(history, user_msg);

    /* Serialize */
    char *json_str = cJSON_PrintUnformatted(history);
    cJSON_Delete(history);

    if (json_str) {
        strncpy(buf, json_str, size - 1);
        buf[size - 1] = '\0';
        free(json_str);
    } else {
        snprintf(buf, size, "[{\"role\":\"user\",\"content\":\"%s\"}]", user_message);
    }

    return ESP_OK;
}
