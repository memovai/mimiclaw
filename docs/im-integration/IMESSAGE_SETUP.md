# iMessage Configuration Guide

This guide walks through setting up iMessage to work with MimiClaw, turning your ESP32-S3 into an iMessage-connected AI assistant via the [Photon](https://photon.codes) proxy service.

## Table of Contents

- [Overview](#overview)
- [Prerequisites](#prerequisites)
- [Step 1: Get Photon Credentials](#step-1-get-photon-credentials)
- [Step 2: Configure MimiClaw](#step-2-configure-mimiclaw)
- [Step 3: Verify and Test](#step-3-verify-and-test)
- [Architecture](#architecture)
- [CLI Commands](#cli-commands)
- [Troubleshooting](#troubleshooting)
- [References](#references)

## Overview

MimiClaw supports iMessage as a messaging channel alongside Telegram, Feishu, and WebSocket. The iMessage integration uses:

- **HTTP polling** — the ESP32 polls the Photon proxy every 5 seconds for new messages
- **REST send API** — MimiClaw sends replies via `POST /send` through the proxy
- **Bearer token auth** — automatic token generation from `Base64("server_url|api_key")`

Both **direct messages (1:1)** and **group chats** are supported.

## Prerequisites

- A [Photon](https://photon.codes) account with an iMessage server URL and API key (see [Advanced iMessage Kit](https://github.com/photon-hq/advanced-imessage-kit) for setup)
- MimiClaw flashed on an ESP32-S3 with WiFi connectivity
- No inbound port forwarding required — iMessage uses outbound HTTP polling, not webhooks

## Step 1: Get Photon Credentials

1. Sign up at [Photon](https://photon.codes)
2. Follow the [Advanced iMessage Kit](https://github.com/photon-hq/advanced-imessage-kit) instructions to set up your iMessage server
3. After setup, you will have two credentials:
   - **Server URL** — your upstream iMessage Kit server (e.g., `https://xxxxx.imsgd.photon.codes`)
   - **API Key** — your Photon API key

> **Important:** Save both values. You will need them to configure MimiClaw.

All REST calls are routed through a centrally-hosted proxy at `https://imessage-swagger.photon.codes` (configurable). Your server URL and API key are only encoded inside the Bearer token — they are never sent as plain-text query parameters.

## Step 2: Configure MimiClaw

You need to provide the **Server URL** and **API Key** to MimiClaw. Optionally, you can override the default REST proxy URL.

### Option 1: Build-time Configuration

1. Copy the secrets template if you haven't already:

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

2. Edit `main/mimi_secrets.h`:

```c
#define MIMI_SECRET_IMSG_SERVER_URL "https://xxxxx.imsgd.photon.codes"              // upstream iMessage Kit server
#define MIMI_SECRET_IMSG_API_KEY    "your-api-key"                                  // API key from Photon
#define MIMI_SECRET_IMSG_PROXY_URL  "https://imessage-swagger.photon.codes"         // REST proxy (optional, has default)
```

3. Rebuild and flash:

```bash
idf.py fullclean && idf.py build
idf.py -p PORT flash monitor
```

### Option 2: Runtime Configuration via Serial CLI

Connect to the serial console and run:

```
mimi> set_imsg_creds https://xxxxx.imsgd.photon.codes your-api-key
```

To also set a custom proxy URL:

```
mimi> set_imsg_creds https://xxxxx.imsgd.photon.codes your-api-key https://custom-proxy.example.com
```

This saves credentials to NVS flash immediately — no rebuild needed. The poller restarts automatically with the new credentials.

### Verify Configuration

```
mimi> config_show
```

You should see `imsg_server_url: https://****` and `imsg_api_key: ****` in the output.

## Step 3: Verify and Test

1. Ensure the ESP32 is connected to WiFi: run `wifi_status` in the serial console
2. Send an iMessage to the address associated with your Photon server
3. Check the ESP32 serial output — you should see the message being received and processed
4. The bot should reply through iMessage

For group chats:

1. Messages from group chats are identified by a `chat` field (e.g., `"group:xxx"`)
2. The bot processes and replies to the group automatically
3. Own messages (where `from` is `"me"`) are filtered to prevent self-reply loops

## Architecture

```
iMessage Proxy (Photon)
    ^
    |  GET /messages?limit=20&sort=desc  (poll every 5s)
    |  POST /send
    |
[ESP32 HTTP Client]
    |
    |  message_bus_push_inbound()
    v
[Message Bus] ──> [Agent Loop] ──> [Message Bus]
                   (Claude/GPT)         |
                                        |  outbound dispatch
                                        v
                              [imessage_send_message()]
                                        |
                                        |  POST /send
                                        v
                                  iMessage Proxy (Photon)
```

### Key Components

| Component | Description |
|-----------|-------------|
| **HTTP Poller** | Polls the proxy every 5 seconds for new messages on Core 0 |
| **Message Sender** | Sends text messages via REST API with auto-chunking (4096 chars per message) |
| **Deduplication** | FNV-1a hash ring buffer (64 entries) prevents processing duplicate messages |
| **High-water Mark** | Tracks the newest message timestamp to skip history on boot |
| **Auth Token** | Automatically generated as `Base64("server_url|api_key")` |

### Configuration Constants

These can be found in `main/mimi_config.h`:

| Constant | Default | Description |
|----------|---------|-------------|
| `MIMI_IMSG_MAX_MSG_LEN` | 4096 | Max message length per chunk |
| `MIMI_IMSG_POLL_STACK` | 16 KB | Polling task stack size |
| `MIMI_IMSG_POLL_PRIO` | 5 | Polling task priority |
| `MIMI_IMSG_POLL_CORE` | 0 | Polling task pinned to Core 0 |
| `MIMI_IMSG_POLL_INTERVAL_MS` | 5000 | Polling interval (5 seconds) |
| `MIMI_IMSG_HTTP_TIMEOUT_MS` | 15000 | HTTP request timeout (15 seconds) |

## CLI Commands

| Command | Description |
|---------|-------------|
| `set_imsg_creds <server_url> <api_key> [proxy_url]` | Save iMessage credentials to NVS |
| `imsg_send <to> <text>` | Send an iMessage directly (e.g., `imsg_send user@icloud.com "hello"`) |
| `config_show` | Show all configuration (including iMessage, masked) |
| `config_reset` | Clear all NVS config, revert to build-time defaults |

## Troubleshooting

### Bot doesn't receive messages

1. **Check credentials**: `config_show` should show `imsg_server_url` and `imsg_api_key`
2. **Check WiFi**: Run `wifi_status` to confirm network connectivity
3. **Check serial output**: Look for `imessage: poll` log entries — if absent, the poller may not have started
4. **Check proxy reachability**: Ensure the ESP32 can reach `https://imessage-swagger.photon.codes` (check proxy settings if needed)

### Bot doesn't send replies

- Verify the Photon server is running and the API key is valid
- Check serial output for `imessage_send_message` errors
- If using a network proxy (`set_proxy`), ensure it allows HTTPS to the Photon proxy URL

### "no prior inbound message" error

Apple silently drops outbound iMessages to addresses that have never contacted your server first (spam filtering). MimiClaw enforces this: if no message has been received from a given address since boot, `imessage_send_message()` returns `ESP_ERR_NOT_ALLOWED`.

- The recipient must send the first message to your iMessage account
- The known-contacts set is persisted in NVS and survives reboots — once a contact has messaged the bot, outbound sends to that address will work across power cycles
- Changing credentials via `set_imsg_creds` clears the persisted set (new account = fresh contacts)
- The `imsg_send` CLI command is also subject to this check

### "timeout" or slow responses

- The HTTP timeout is 15 seconds by default (`MIMI_IMSG_HTTP_TIMEOUT_MS`)
- If you are behind a restrictive network, configure an HTTP CONNECT or SOCKS5 proxy via `set_proxy`
- TLS handshakes on the ESP32 can take a few seconds — this is normal

### Messages are duplicated

- MimiClaw uses FNV-1a hash deduplication with a 64-entry ring buffer
- If you see duplicates, the ring buffer may have wrapped — this is unlikely under normal polling intervals
- Check for multiple MimiClaw devices polling the same Photon server

### Messages are truncated

iMessage messages are chunked at 4096 characters per message. MimiClaw automatically splits long responses. If you see issues, check the serial output for chunking errors.

### Credentials were set but bot still doesn't work

- After setting credentials via CLI, the poller re-initializes automatically
- If issues persist, restart the device: `restart`

## References

- [Photon](https://photon.codes) — iMessage proxy service
- [Advanced iMessage HTTP Proxy](https://github.com/photon-hq/advanced-imessage-http-proxy) — proxy server source
- [Advanced iMessage Kit](https://github.com/photon-hq/advanced-imessage-kit) — iMessage server setup guide
