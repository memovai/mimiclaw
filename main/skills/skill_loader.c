#include "skills/skill_loader.h"
#include "mimi_config.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include "esp_log.h"

static const char *TAG = "skills";

/* ── Built-in skill contents ─────────────────────────────────── */

#define BUILTIN_WEATHER \
    "# Weather\n" \
    "\n" \
    "Get current weather and forecasts using web_search.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks about weather, temperature, or forecasts.\n" \
    "\n" \
    "## How to use\n" \
    "1. Use get_current_time to know the current date\n" \
    "2. Use web_search with a query like \"weather in [city] today\"\n" \
    "3. Extract temperature, conditions, and forecast from results\n" \
    "4. Present in a concise, friendly format\n" \
    "\n" \
    "## Example\n" \
    "User: \"What's the weather in Tokyo?\"\n" \
    "→ get_current_time\n" \
    "→ web_search \"weather Tokyo today February 2026\"\n" \
    "→ \"Tokyo: 8°C, partly cloudy. High 12°C, low 4°C. Light wind from the north.\"\n"

#define BUILTIN_DAILY_BRIEFING \
    "# Daily Briefing\n" \
    "\n" \
    "Compile a personalized daily briefing for the user.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks for a daily briefing, morning update, or \"what's new today\".\n" \
    "Also useful as a heartbeat/cron task.\n" \
    "\n" \
    "## How to use\n" \
    "1. Use get_current_time for today's date\n" \
    "2. Read " MIMI_SPIFFS_MEMORY_DIR "/MEMORY.md for user preferences and context\n" \
    "3. Read today's daily note if it exists\n" \
    "4. Use web_search for relevant news based on user interests\n" \
    "5. Compile a concise briefing covering:\n" \
    "   - Date and time\n" \
    "   - Weather (if location known from USER.md)\n" \
    "   - Relevant news/updates based on user interests\n" \
    "   - Any pending tasks from memory\n" \
    "   - Any scheduled cron jobs\n" \
    "\n" \
    "## Format\n" \
    "Keep it brief — 5-10 bullet points max. Use the user's preferred language.\n"

#define BUILTIN_ARXIV_SEARCH \
    "# ArXiv Search\n" \
    "\n" \
    "Search for academic papers on ArXiv by keywords using the http_request tool.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks to find academic papers, research articles, preprints, or scientific publications.\n" \
    "Also when the user mentions ArXiv or asks about recent research on a topic.\n" \
    "\n" \
    "## How to use\n" \
    "1. Identify the search keywords from the user's query\n" \
    "2. Build the ArXiv API query URL:\n" \
    "   - Base URL: `https://export.arxiv.org/api/query`\n" \
    "   - Add `search_query=` with keywords joined by `+AND+` (URL-encoded spaces as `+`)\n" \
    "   - Use field prefixes: `all:` (any field), `ti:` (title), `au:` (author), `abs:` (abstract), `cat:` (category)\n" \
    "   - Add `&start=0&max_results=5` to limit results\n" \
    "   - Add `&sortBy=submittedDate&sortOrder=descending` for newest first\n" \
    "3. Use http_request tool with method GET and the constructed URL\n" \
    "4. Parse the Atom XML response — each `<entry>` contains:\n" \
    "   - `<title>`: paper title\n" \
    "   - `<summary>`: abstract\n" \
    "   - `<author><name>`: author names\n" \
    "   - `<link>` with `title=\"pdf\"`: PDF link\n" \
    "   - `<published>`: publication date\n" \
    "5. Present results in a clear format: title, authors, date, abstract snippet, and link\n" \
    "\n" \
    "## Example\n" \
    "User: \"Find recent papers on large language models\"\n" \
    "→ http_request url=\"https://export.arxiv.org/api/query?search_query=all:large+AND+all:language+AND+all:models&start=0&max_results=5&sortBy=submittedDate&sortOrder=descending\" method=\"GET\"\n" \
    "→ Parse the XML response and list papers with title, authors, date, and link\n" \
    "\n" \
    "User: \"Search ArXiv for papers by Yann LeCun on deep learning\"\n" \
    "→ http_request url=\"https://export.arxiv.org/api/query?search_query=au:LeCun+AND+all:deep+learning&start=0&max_results=5&sortBy=submittedDate&sortOrder=descending\" method=\"GET\"\n"

#define BUILTIN_SKILL_CREATOR \
    "# Skill Creator\n" \
    "\n" \
    "Create new skills for MimiClaw.\n" \
    "\n" \
    "## When to use\n" \
    "When the user asks to create a new skill, teach the bot something, or add a new capability.\n" \
    "\n" \
    "## How to create a skill\n" \
    "1. Choose a short, descriptive name (lowercase, hyphens ok)\n" \
    "2. Write a SKILL.md file with this structure:\n" \
    "   - `# Title` — clear name\n" \
    "   - Brief description paragraph\n" \
    "   - `## When to use` — trigger conditions\n" \
    "   - `## How to use` — step-by-step instructions\n" \
    "   - `## Example` — concrete example (optional but helpful)\n" \
    "3. Save to `" MIMI_SKILLS_PREFIX "<name>.md` using write_file\n" \
    "4. The skill will be automatically available after the next conversation\n" \
    "\n" \
    "## Best practices\n" \
    "- Keep skills concise — the context window is limited\n" \
    "- Focus on WHAT to do, not HOW (the agent is smart)\n" \
    "- Include specific tool calls the agent should use\n" \
    "- Test by asking the agent to use the new skill\n" \
    "\n" \
    "## Example\n" \
    "To create a \"translate\" skill:\n" \
    "write_file path=\"" MIMI_SKILLS_PREFIX "translate.md\" content=\"# Translate\\n\\nTranslate text between languages.\\n\\n" \
    "## When to use\\nWhen the user asks to translate text.\\n\\n" \
    "## How to use\\n1. Identify source and target languages\\n" \
    "2. Translate directly using your language knowledge\\n" \
    "3. For specialized terms, use web_search to verify\\n\"\n"

/* Built-in skill registry */
typedef struct {
    const char *filename;   /* e.g. "weather" */
    const char *content;
} builtin_skill_t;

static const builtin_skill_t s_builtins[] = {
    { "weather",        BUILTIN_WEATHER        },
    { "daily-briefing", BUILTIN_DAILY_BRIEFING },
    { "arxiv-search",   BUILTIN_ARXIV_SEARCH   },
    { "skill-creator",  BUILTIN_SKILL_CREATOR  },
};

#define NUM_BUILTINS (sizeof(s_builtins) / sizeof(s_builtins[0]))

/* ── Install built-in skills if missing ──────────────────────── */

static void install_builtin(const builtin_skill_t *skill)
{
    char path[64];
    snprintf(path, sizeof(path), "%s%s.md", MIMI_SKILLS_PREFIX, skill->filename);

    /* Check if already exists */
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        ESP_LOGD(TAG, "Skill exists: %s", path);
        return;
    }

    /* Write built-in skill */
    f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot write skill: %s", path);
        return;
    }

    fputs(skill->content, f);
    fclose(f);
    ESP_LOGI(TAG, "Installed built-in skill: %s", path);
}

esp_err_t skill_loader_init(void)
{
    ESP_LOGI(TAG, "Initializing skills system");

    for (size_t i = 0; i < NUM_BUILTINS; i++) {
        install_builtin(&s_builtins[i]);
    }

    ESP_LOGI(TAG, "Skills system ready (%d built-in)", (int)NUM_BUILTINS);
    return ESP_OK;
}

/* ── Build skills summary for system prompt ──────────────────── */

/**
 * Parse first line as title: expects "# Title"
 * Returns pointer past "# " or the line itself if no prefix.
 */
static const char *extract_title(const char *line, size_t len, char *out, size_t out_size)
{
    const char *start = line;
    if (len >= 2 && line[0] == '#' && line[1] == ' ') {
        start = line + 2;
        len -= 2;
    }

    /* Trim trailing whitespace/newline */
    while (len > 0 && (start[len - 1] == '\n' || start[len - 1] == '\r' || start[len - 1] == ' ')) {
        len--;
    }

    size_t copy = len < out_size - 1 ? len : out_size - 1;
    memcpy(out, start, copy);
    out[copy] = '\0';
    return out;
}

/**
 * Extract description: text between the first line and the first blank line.
 */
static void extract_description(FILE *f, char *out, size_t out_size)
{
    size_t off = 0;
    char line[256];

    while (fgets(line, sizeof(line), f) && off < out_size - 1) {
        size_t len = strlen(line);

        /* Stop at blank line or section header */
        if (len == 0 || (len == 1 && line[0] == '\n') ||
            (len >= 2 && line[0] == '#' && line[1] == '#')) {
            break;
        }

        /* Skip leading blank lines */
        if (off == 0 && line[0] == '\n') continue;

        /* Trim trailing newline for concatenation */
        if (line[len - 1] == '\n') {
            line[len - 1] = ' ';
        }

        size_t copy = len < out_size - off - 1 ? len : out_size - off - 1;
        memcpy(out + off, line, copy);
        off += copy;
    }

    /* Trim trailing space */
    while (off > 0 && out[off - 1] == ' ') off--;
    out[off] = '\0';
}

size_t skill_loader_build_summary(char *buf, size_t size)
{
    DIR *dir = opendir(MIMI_SPIFFS_BASE);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open SPIFFS for skill enumeration");
        buf[0] = '\0';
        return 0;
    }

    size_t off = 0;
    struct dirent *ent;
    /* SPIFFS readdir returns filenames relative to the mount point (e.g. "skills/weather.md").
       We match entries that start with "skills/" and end with ".md". */
    const char *skills_subdir = "skills/";
    const size_t subdir_len = strlen(skills_subdir);

    while ((ent = readdir(dir)) != NULL && off < size - 1) {
        const char *name = ent->d_name;

        /* Match files under skills/ with .md extension */
        if (strncmp(name, skills_subdir, subdir_len) != 0) continue;

        size_t name_len = strlen(name);
        if (name_len < subdir_len + 4) continue;  /* at least "skills/x.md" */
        if (strcmp(name + name_len - 3, ".md") != 0) continue;

        /* Build full path */
        char full_path[296];
        snprintf(full_path, sizeof(full_path), "%s/%s", MIMI_SPIFFS_BASE, name);

        FILE *f = fopen(full_path, "r");
        if (!f) continue;

        /* Read first line for title */
        char first_line[128];
        if (!fgets(first_line, sizeof(first_line), f)) {
            fclose(f);
            continue;
        }

        char title[64];
        extract_title(first_line, strlen(first_line), title, sizeof(title));

        /* Read description (until blank line) */
        char desc[256];
        extract_description(f, desc, sizeof(desc));
        fclose(f);

        /* Append to summary */
        off += snprintf(buf + off, size - off,
            "- **%s**: %s (read with: read_file %s)\n",
            title, desc, full_path);
    }

    closedir(dir);

    buf[off] = '\0';
    ESP_LOGI(TAG, "Skills summary: %d bytes", (int)off);
    return off;
}
