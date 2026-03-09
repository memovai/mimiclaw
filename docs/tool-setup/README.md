# Tool Setup Guides

Configuration guides for MimiClaw's external service integrations.

## Guides

| Guide | Service | Description |
|-------|---------|-------------|
| [Tavily Setup](TAVILY_SETUP.md) | [Tavily](https://tavily.com) | Web search API — preferred search provider for the `web_search` tool |

## Overview

MimiClaw integrates with external services to extend its capabilities. Each guide below walks through obtaining API credentials, configuring MimiClaw (build-time or runtime), and verifying the integration.

All credentials can be set in two ways:

1. **Build-time** — define in `main/mimi_secrets.h` and rebuild
2. **Runtime** — use serial CLI commands (saved to NVS flash, no rebuild needed)

See [mimi_secrets.h.example](../../main/mimi_secrets.h.example) for the full list of configurable secrets.
