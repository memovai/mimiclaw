#include "tools/tool_search_notes.h"
#include "mimi_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_search_notes";

#define MAX_QUERY_WORDS  10
#define MAX_RESULTS      64
#define READ_CAP         4096

typedef struct {
    char path[128];
    int  matches;
} note_result_t;

/**
 * Case-insensitive substring search.
 * Returns pointer to first occurrence, or NULL.
 */
static const char *contains_nocase(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle) return NULL;
    for (const char *p = haystack; *p; p++) {
        if (tolower((unsigned char)*p) == tolower((unsigned char)*needle)) {
            const char *h = p;
            const char *n = needle;
            while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
                h++;
                n++;
            }
            if (!*n) return p;
        }
    }
    return NULL;
}

esp_err_t tool_search_notes_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *query = cJSON_GetStringValue(cJSON_GetObjectItem(root, "query"));
    if (!query || !*query) {
        snprintf(output, output_size, "Error: missing or empty 'query' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Split query into words */
    char qbuf[256];
    strncpy(qbuf, query, sizeof(qbuf) - 1);
    qbuf[sizeof(qbuf) - 1] = '\0';

    char *words[MAX_QUERY_WORDS];
    int word_count = 0;
    char *tok = strtok(qbuf, " ");
    while (tok && word_count < MAX_QUERY_WORDS) {
        words[word_count++] = tok;
        tok = strtok(NULL, " ");
    }

    if (word_count == 0) {
        snprintf(output, output_size, "Error: query contains no words");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Scan notes directory */
    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    if (!dir) {
        snprintf(output, output_size, "Error: cannot open %s directory", MIMI_SPIFFS_BASE);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    note_result_t results[MAX_RESULTS];
    int result_count = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && result_count < MAX_RESULTS) {
        /* SPIFFS stores flat names like "memory/2026-02-15.md" */
        const char *name = ent->d_name;

        /* Must be in memory/ subdirectory and end with .md */
        if (strncmp(name, "memory/", 7) != 0) continue;
        size_t nlen = strlen(name);
        if (nlen < 4 || strcmp(name + nlen - 3, ".md") != 0) continue;

        /* Skip MEMORY.md */
        if (strcmp(name + 7, "MEMORY.md") == 0) continue;

        /* Build full path — SPIFFS names are short; skip if somehow too long */
        char full_path[128];
        int plen = snprintf(full_path, sizeof(full_path), "%s/%s", MIMI_SPIFFS_BASE, name);
        if (plen < 0 || (size_t)plen >= sizeof(full_path)) continue;

        /* Read file content (up to READ_CAP bytes) */
        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        char *buf = malloc(READ_CAP + 1);
        if (!buf) {
            fclose(f);
            continue;
        }

        size_t n = fread(buf, 1, READ_CAP, f);
        buf[n] = '\0';
        fclose(f);

        /* Count distinct query words that appear */
        int matches = 0;
        for (int i = 0; i < word_count; i++) {
            if (contains_nocase(buf, words[i])) {
                matches++;
            }
        }
        free(buf);

        if (matches > 0) {
            strncpy(results[result_count].path, full_path, sizeof(results[result_count].path) - 1);
            results[result_count].path[sizeof(results[result_count].path) - 1] = '\0';
            results[result_count].matches = matches;
            result_count++;
        }
    }
    closedir(dir);

    /* Sort by match count descending (simple insertion sort) */
    for (int i = 1; i < result_count; i++) {
        note_result_t tmp = results[i];
        int j = i - 1;
        while (j >= 0 && results[j].matches < tmp.matches) {
            results[j + 1] = results[j];
            j--;
        }
        results[j + 1] = tmp;
    }

    /* Format output */
    if (result_count == 0) {
        snprintf(output, output_size, "No notes matching \"%s\".", query);
        ESP_LOGI(TAG, "search_notes: 0 matches for \"%s\"", query);
        cJSON_Delete(root);
        return ESP_OK;
    }

    size_t off = 0;
    off += snprintf(output + off, output_size - off,
                    "Found %d notes matching \"%s\" (%d words):\n\n",
                    result_count, query, word_count);

    for (int i = 0; i < result_count && off < output_size - 1; i++) {
        off += snprintf(output + off, output_size - off,
                        "%d. %s (%d/%d words)\n",
                        i + 1, results[i].path, results[i].matches, word_count);
    }

    ESP_LOGI(TAG, "search_notes: %d matches for \"%s\"", result_count, query);
    cJSON_Delete(root);
    return ESP_OK;
}
