#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Search daily notes for keywords.
 * Input JSON: {"query": "word1 word2 ..."}
 * Returns matching filenames ranked by number of distinct query words found.
 */
esp_err_t tool_search_notes_execute(const char *input_json, char *output, size_t output_size);
