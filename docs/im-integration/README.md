# IM Integration Guides

Configuration guides for MimiClaw's instant messaging channel integrations.

## Guides

| Guide | Service | Description |
|-------|---------|-------------|
| [Feishu Setup](FEISHU_SETUP.md) | [Feishu / Lark](https://open.feishu.cn/) | Feishu bot channel — receive and send messages via Feishu |

## Overview

MimiClaw supports multiple IM channels for interacting with the AI agent. Each guide below walks through obtaining API credentials, configuring MimiClaw (build-time or runtime), and verifying the integration.

All credentials can be set in two ways:

1. **Build-time** — define in `main/mimi_secrets.h` and rebuild
2. **Runtime** — use serial CLI commands (saved to NVS flash, no rebuild needed)

See [mimi_secrets.h.example](../../main/mimi_secrets.h.example) for the full list of configurable secrets.
