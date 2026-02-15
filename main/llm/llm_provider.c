#include "llm_provider.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>

#define MAX_PROVIDERS 8

static const llm_provider_t *s_providers[MAX_PROVIDERS] = {0};
static int s_provider_count = 0;
static const llm_provider_t *s_default_provider = NULL;

esp_err_t llm_provider_register(const llm_provider_t *provider)
{
    if (!provider || s_provider_count >= MAX_PROVIDERS) {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < s_provider_count; i++) {
        if (strcmp(s_providers[i]->name, provider->name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    s_providers[s_provider_count++] = provider;

    if (s_default_provider == NULL ||
        strcmp(provider->name, MIMI_LLM_PROVIDER_DEFAULT) == 0) {
        s_default_provider = provider;
    }

    return ESP_OK;
}

const llm_provider_t *llm_provider_get(const char *name)
{
    if (!name) return s_default_provider;

    for (int i = 0; i < s_provider_count; i++) {
        if (strcmp(s_providers[i]->name, name) == 0) {
            return s_providers[i];
        }
    }
    return NULL;
}

const llm_provider_t *llm_provider_get_default(void)
{
    return s_default_provider;
}

void llm_provider_set_default(const char *name)
{
    const llm_provider_t *p = llm_provider_get(name);
    if (p) {
        s_default_provider = p;
    }
}
