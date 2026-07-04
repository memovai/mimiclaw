# MimiClaw Architecture

> ESP32-S3 AI Agent firmware in C/FreeRTOS, running without Linux, Node.js, or a host server.

## System Overview

MimiClaw receives messages from Telegram, Feishu/Lark, or the local WebSocket gateway. Each message is pushed into a FreeRTOS inbound queue, processed by the agent loop, optionally handled through tools, and then routed back through the outbound queue.

```text
Telegram / Feishu / WebSocket
        |
        v
Inbound queue -> Agent loop -> LLM proxy -> Anthropic or OpenAI
                     |
                     v
      Tools: search, time, files, cron, GPIO
                     |
                     v
Outbound queue -> Telegram / Feishu / WebSocket / system log
```

Persistent state lives on SPIFFS:

- `/spiffs/config/SOUL.md` and `/spiffs/config/USER.md`
- `/spiffs/memory/MEMORY.md` and `/spiffs/memory/YYYY-MM-DD.md`
- `/spiffs/sessions/<chat_id>.jsonl`
- `/spiffs/skills/*.md`
- `/spiffs/cron.json`
- `/spiffs/HEARTBEAT.md`

## Data Flow

1. A channel receives a text message and wraps it in `mimi_msg_t`.
2. The message is pushed to the inbound queue.
3. The agent loop builds context from config files, long-term memory, recent daily notes, skills, session history, and tool guidance.
4. The LLM proxy calls the configured provider with tool schemas.
5. If the model requests tools, the agent executes them and sends tool results back into the same LLM turn.
6. The final response is saved to the session history and pushed to the outbound queue.
7. The outbound dispatcher sends the response through Telegram, Feishu, WebSocket, or the system log.

## Module Map

```text
main/
├── mimi.c                    app_main(), startup order, outbound dispatch
├── mimi_config.h             compile-time constants and default secret include
├── bus/                      FreeRTOS inbound/outbound queues
├── wifi/                     WiFi STA lifecycle
├── channels/
│   ├── telegram/             Telegram long polling and sendMessage
│   └── feishu/               Feishu/Lark WebSocket long connection and send API
├── llm/                      Anthropic/OpenAI HTTPS proxy and tool-call parsing
├── agent/                    ReAct loop and context builder
├── tools/                    search, time, SPIFFS files, cron, GPIO tools
├── memory/                   MEMORY.md, daily notes, and JSONL sessions
├── skills/                   SPIFFS markdown skill summary loader
├── cron/                     persistent at/every scheduled agent triggers
├── heartbeat/                periodic HEARTBEAT.md checks
├── gateway/                  local WebSocket server on port 18789
├── proxy/                    HTTP CONNECT tunnel support
├── cli/                      serial REPL and runtime configuration
├── ota/                      HTTPS OTA wrapper
└── onboard/                  captive portal and local admin config portal
```

## FreeRTOS Tasks

| Task | Core | Priority | Stack | Description |
|---|---:|---:|---:|---|
| `tg_poll` | 0 | 5 | 12 KB | Telegram long polling |
| `feishu_ws` | 0 | 5 | 12 KB | Feishu/Lark WebSocket long connection |
| `agent_loop` | 1 | 6 | 24 KB | Agent turn processing and LLM calls |
| `outbound` | 0 | 5 | 12 KB | Response routing |
| `serial_cli` | 0 | 3 | 4 KB | UART console REPL |
| `cron` | any | 4 | 4 KB | Scheduled job checks |
| `heartbeat` timer | - | - | - | Periodic HEARTBEAT.md check |
| httpd internal tasks | 0 | 5 | IDF-managed | WebSocket/admin HTTP server |
| WiFi/event tasks | 0 | IDF-managed | IDF-managed | WiFi and event handling |

Large buffers for context, history, tool output, and LLM responses are allocated from PSRAM where possible.

## Flash Layout

```text
Offset      Size      Name        Purpose
0x009000    24 KB     nvs         ESP-IDF and runtime app config
0x00F000     8 KB     otadata     OTA boot state
0x011000     4 KB     phy_init    WiFi PHY calibration
0x020000     2 MB     ota_0       Firmware slot A
0x220000     2 MB     ota_1       Firmware slot B
0x420000    12 MB     spiffs      Markdown config, memory, sessions, skills, cron
0xFF0000    64 KB     coredump    Crash dump storage
```

## Configuration

Configuration has two layers:

1. `main/mimi_secrets.h` provides build-time defaults.
2. NVS stores runtime overrides set through the serial CLI or onboarding/admin portal.

Important build-time defaults:

| Define | Description |
|---|---|
| `MIMI_SECRET_WIFI_SSID` / `MIMI_SECRET_WIFI_PASS` | WiFi credentials |
| `MIMI_SECRET_TG_TOKEN` | Telegram bot token |
| `MIMI_SECRET_FEISHU_APP_ID` / `MIMI_SECRET_FEISHU_APP_SECRET` | Feishu/Lark credentials |
| `MIMI_SECRET_API_KEY` | Anthropic or OpenAI API key |
| `MIMI_SECRET_MODEL_PROVIDER` | `anthropic` or `openai` |
| `MIMI_SECRET_MODEL` | Model ID |
| `MIMI_SECRET_PROXY_HOST` / `MIMI_SECRET_PROXY_PORT` | HTTP CONNECT proxy |
| `MIMI_SECRET_TAVILY_KEY` | Tavily Search key, preferred when present |
| `MIMI_SECRET_SEARCH_KEY` | Brave Search key fallback |

Runtime configuration commands include `wifi_set`, `set_tg_token`, `set_feishu_creds`, `set_api_key`, `set_model_provider`, `set_model`, `set_proxy`, `clear_proxy`, `set_search_key`, `set_tavily_key`, `config_show`, and `config_reset`.

## Message Bus

The internal bus uses two FreeRTOS queues carrying `mimi_msg_t`:

```c
typedef struct {
    char channel[16];   /* "telegram", "feishu", "websocket", "cli", "system" */
    char chat_id[96];   /* Telegram/Feishu chat_id, open_id, or WS client id */
    char *content;      /* heap-allocated text; ownership transfers through queues */
} mimi_msg_t;
```

The inbound queue carries channel messages to the agent loop. The outbound queue carries agent responses to the dispatcher.

## Tools

Tools are registered in `tools/tool_registry.c` and exposed to the configured LLM provider:

| Tool | Purpose |
|---|---|
| `web_search` | Search through Tavily when configured, otherwise Brave |
| `get_current_time` | Fetch current time and update the device clock |
| `read_file` | Read SPIFFS files under `/spiffs/` |
| `write_file` | Write SPIFFS files under `/spiffs/` |
| `edit_file` | Replace the first matching string in a SPIFFS file |
| `list_dir` | List SPIFFS entries, optionally by prefix |
| `cron_add` | Add a recurring interval or one-shot epoch job |
| `cron_list` | List scheduled jobs |
| `cron_remove` | Remove a scheduled job |
| `gpio_write` | Set an allowed GPIO pin high or low |
| `gpio_read` | Read one allowed GPIO pin |
| `gpio_read_all` | Read all allowed GPIO pins |

File tools intentionally restrict paths to `/spiffs/` and reject `..` traversal.

## LLM Providers

The LLM proxy supports:

- Anthropic Messages API: `https://api.anthropic.com/v1/messages`
- OpenAI Chat Completions API: `https://api.openai.com/v1/chat/completions`

Anthropic requests use a top-level `system` field and Anthropic tool-use blocks. OpenAI requests convert the system prompt into a system message and convert tool schemas to OpenAI's `tools` format. Calls are non-streaming; the WebSocket gateway does not currently stream tokens.

## Startup Sequence

```text
app_main()
  ├── init_nvs()
  ├── esp_event_loop_create_default()
  ├── init_spiffs()
  ├── message_bus_init()
  ├── memory_store_init()
  ├── skill_loader_init()
  ├── session_mgr_init()
  ├── wifi_manager_init()
  ├── http_proxy_init()
  ├── telegram_bot_init()
  ├── feishu_bot_init()
  ├── llm_proxy_init()
  ├── tool_registry_init()
  ├── cron_service_init()
  ├── heartbeat_init()
  ├── agent_loop_init()
  ├── serial_cli_init()
  ├── wifi_manager_start()
  ├── if WiFi unavailable: wifi_onboard_start(captive)
  └── if WiFi connected:
      ├── wifi_onboard_start(admin)
      ├── outbound dispatch task
      ├── agent_loop_start()
      ├── telegram_bot_start()
      ├── feishu_bot_start()
      ├── cron_service_start()
      ├── heartbeat_start()
      └── ws_server_start()
```

The serial CLI starts before WiFi so the device can still be inspected and reconfigured when network setup fails.

## Serial CLI

The CLI supports runtime configuration and diagnostics:

| Command | Description |
|---|---|
| `wifi_set <SSID> <PASS>` | Save WiFi credentials |
| `set_tg_token <TOKEN>` | Save Telegram token |
| `set_feishu_creds <ID> <SECRET>` | Save Feishu credentials |
| `set_api_key <KEY>` | Save LLM API key |
| `set_model_provider <PROVIDER>` | Select `anthropic` or `openai` |
| `set_model <MODEL>` | Save model name |
| `set_proxy <HOST> <PORT>` | Save proxy settings |
| `clear_proxy` | Clear proxy settings |
| `set_search_key <KEY>` | Save Brave Search key |
| `set_tavily_key <KEY>` | Save Tavily key |
| `config_show` | Show masked config |
| `config_reset` | Clear runtime app config |
| `wifi_status` | Show WiFi state |
| `memory_read` / `memory_write` | Inspect or overwrite `MEMORY.md` |
| `session_list` / `session_clear` | Inspect or clear session files |
| `heartbeat_trigger` | Manually trigger HEARTBEAT.md processing |
| `cron_start` | Start the cron scheduler |
| `heap_info` | Show internal RAM and PSRAM |
| `restart` | Reboot the device |

## Nanobot Reference Mapping

| Nanobot Module | MimiClaw Equivalent | Notes |
|---|---|---|
| `agent/loop.py` | `agent/agent_loop.c` | ReAct loop with tool use |
| `agent/context.py` | `agent/context_builder.c` | Config, memory, skills, and tool guidance |
| `agent/memory.py` | `memory/memory_store.c` | `MEMORY.md` and daily notes |
| `session/manager.py` | `memory/session_mgr.c` | JSONL per chat/session |
| `channels/telegram.py` | `channels/telegram/telegram_bot.c` | Raw HTTP Telegram API |
| `channels/feishu.py` | `channels/feishu/feishu_bot.c` | Feishu/Lark long connection |
| `bus/events.py` + `queue.py` | `bus/message_bus.c` | FreeRTOS queues |
| `providers/litellm_provider.py` | `llm/llm_proxy.c` | Direct Anthropic/OpenAI support |
| `config/schema.py` | `mimi_config.h`, `mimi_secrets.h`, NVS | Build-time defaults plus runtime overrides |
| `cli/commands.py` | `cli/serial_cli.c` | `esp_console` REPL |
| `agent/tools/*` | `tools/tool_registry.c`, `tools/tool_*.c` | Search, time, files, cron, GPIO |
| `agent/skills.py` | `skills/skill_loader.c` | Simplified SPIFFS markdown skills |
| `cron/service.py` | `cron/cron_service.c` | Simplified at/every scheduler |
| `heartbeat/service.py` | `heartbeat/heartbeat.c` | Periodic `HEARTBEAT.md` checks |
| `agent/subagent.py` | Not implemented | Keep as future work only if needed |
