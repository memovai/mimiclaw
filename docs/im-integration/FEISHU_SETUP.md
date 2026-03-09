# Feishu / Lark Bot Configuration Guide

This guide walks through setting up a Feishu (or Lark) bot to work with MimiClaw, turning your ESP32-S3 into a Feishu-connected AI assistant.

## Table of Contents

- [Overview](#overview)
- [Prerequisites](#prerequisites)
- [Step 1: Create a Feishu App](#step-1-create-a-feishu-app)
- [Step 2: Configure App Permissions](#step-2-configure-app-permissions)
- [Step 3: Set Up Event Subscription](#step-3-set-up-event-subscription)
- [Step 4: Configure MimiClaw](#step-4-configure-mimiclaw)
- [Step 5: Network Setup](#step-5-network-setup)
- [Step 6: Publish and Test](#step-6-publish-and-test)
- [Architecture](#architecture)
- [CLI Commands](#cli-commands)
- [Troubleshooting](#troubleshooting)
- [References](#references)

## Overview

MimiClaw supports Feishu as a messaging channel alongside Telegram and WebSocket. The Feishu integration uses:

- **Webhook receiver** — the ESP32 runs an HTTP server on port 18790 to receive messages from Feishu
- **Send API** — MimiClaw sends replies via Feishu's REST API (`/im/v1/messages`)
- **Tenant access token** — automatic token management with background refresh

Both **direct messages (P2P)** and **group chats** are supported.

## Prerequisites

- A Feishu account (sign up at [feishu.cn](https://www.feishu.cn)) or a Lark account ([larksuite.com](https://www.larksuite.com))
- Admin access to create apps on [Feishu Open Platform](https://open.feishu.cn/) (or [Lark Developer](https://open.larksuite.com/))
- MimiClaw flashed on an ESP32-S3 with network access
- The ESP32 must be reachable from the internet (see [Network Setup](#step-5-network-setup))

## Step 1: Create a Feishu App

1. Go to [Feishu Open Platform](https://open.feishu.cn/) and sign in
2. Click **Create Custom App** (or "Create App" on Lark)
3. Fill in the app details:
   - **App Name**: Choose a name (e.g., "MimiClaw Bot")
   - **App Description**: Brief description of your bot
   - **App Icon**: Upload an icon (optional)
4. After creation, you will see your **App ID** and **App Secret** on the app's **Credentials & Basic Info** page

> **Important:** Save the **App ID** (`cli_xxxxxxxxxxxxxx`) and **App Secret** (`xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx`). You will need these to configure MimiClaw.

## Step 2: Configure App Permissions

In your app's settings, go to **Permissions & Scopes** and add these required permissions:

| Permission | Scope ID | Description |
|-----------|----------|-------------|
| Read/Send messages | `im:message` | Receive and send messages |
| Send messages as bot | `im:message:send_as_bot` | Send messages as the bot identity |

To add permissions:

1. Navigate to **Permissions & Scopes** in the left sidebar
2. Search for each scope ID listed above
3. Click **Add** next to each permission
4. The permissions will take effect after you publish or update the app version

> **Note:** On Lark (international version), the permission names may differ slightly, but the scope IDs are the same.

## Step 3: Set Up Event Subscription

Event subscription allows Feishu to push new messages to your ESP32 in real-time.

### Configure the Webhook URL

1. In your app settings, go to **Event Subscriptions** (or "Events & Callbacks")
2. Set the **Request URL** to:

```
http://<ESP32_IP>:18790/feishu/events
```

Replace `<ESP32_IP>` with your ESP32's public IP or domain name.

3. Click **Save** — Feishu will send a verification challenge to the URL
4. MimiClaw automatically responds to the URL verification challenge, so this should succeed if the ESP32 is reachable

### Subscribe to Events

Add the following event:

| Event | Event ID | Description |
|-------|----------|-------------|
| Receive messages | `im.message.receive_v1` | Triggered when users send messages to the bot |

To add events:

1. In the **Event Subscriptions** page, click **Add Event**
2. Search for `im.message.receive_v1`
3. Select it and click **Confirm**

### Encryption Settings (Optional)

In the event subscription settings, you can optionally configure:

- **Verification Token** — used to verify that events come from Feishu
- **Encrypt Key** — encrypts event payloads

MimiClaw currently does not verify these tokens, so you can leave them empty for simplicity. For production use, consider implementing verification.

## Step 4: Configure MimiClaw

You need to provide the **App ID** and **App Secret** to MimiClaw.

### Option 1: Build-time Configuration

1. Copy the secrets template if you haven't already:

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

2. Edit `main/mimi_secrets.h`:

```c
#define MIMI_SECRET_FEISHU_APP_ID     "cli_xxxxxxxxxxxxxx"
#define MIMI_SECRET_FEISHU_APP_SECRET "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
```

3. Rebuild and flash:

```bash
idf.py fullclean && idf.py build
idf.py -p PORT flash monitor
```

### Option 2: Runtime Configuration via Serial CLI

Connect to the UART (COM) port and run:

```
mimi> set_feishu_creds cli_xxxxxxxxxxxxxx xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
```

This saves credentials to NVS flash immediately — no rebuild needed.

### Verify Configuration

```
mimi> config_show
```

You should see `feishu_app_id: cli_****` and `feishu_app_secret: ****` in the output.

## Step 5: Network Setup

Feishu's servers need to reach your ESP32's webhook endpoint. There are several approaches:

### Option A: Direct Public IP

If your ESP32 is directly accessible from the internet:

```
Webhook URL: http://<PUBLIC_IP>:18790/feishu/events
```

### Option B: Port Forwarding

If the ESP32 is behind a router:

1. Log into your router's admin panel
2. Forward external port (e.g., 18790) to `<ESP32_LOCAL_IP>:18790`
3. Use your router's public IP in the webhook URL

### Option C: Reverse Proxy / Tunnel

For development or when port forwarding isn't possible:

- **ngrok**: `ngrok http <ESP32_IP>:18790`
- **frp**: Configure `frpc.toml` to proxy to the ESP32
- **Cloudflare Tunnel**: Route traffic through Cloudflare

Example with ngrok:

```bash
ngrok http 192.168.1.100:18790
# Use the generated URL: https://xxxx.ngrok.io/feishu/events
```

### Option D: Cloud Server Relay (Production)

For reliable production setups, deploy a lightweight reverse proxy on a cloud server (e.g., Volcengine ECS, AWS EC2) that forwards requests to your ESP32 via a VPN or WireGuard tunnel. This is the approach described in the [Volcengine OpenClaw deployment guide](https://www.volcengine.com/docs/6396/2189942).

> **Note:** Feishu requires the webhook URL to be accessible and respond within 3 seconds. Ensure your network path has low latency.

## Step 6: Publish and Test

### Enable the Bot

1. In your app settings, go to **Bot** in the left sidebar
2. Toggle **Enable Bot** to ON

### Publish the App

1. Go to **App Release** (or "Version Management & Release")
2. Click **Create Version**
3. Set a version number and description
4. Click **Submit for Review** (for enterprise apps) or **Publish** (for personal testing apps)

> **Tip:** For testing, you can use the app in "development mode" without publishing — just add yourself as a test user.

### Test the Bot

1. Open Feishu and search for your bot by name
2. Send a message to the bot in a direct chat
3. Check the ESP32 serial output — you should see the message being received and processed
4. The bot should reply through Feishu

For group chats:

1. Add the bot to a group
2. Mention the bot with `@BotName` followed by your message
3. The bot will process and reply in the group

## Architecture

```
Feishu Cloud
    |
    |  HTTP POST /feishu/events
    |  (im.message.receive_v1)
    v
[ESP32 Webhook Server :18790]
    |
    |  message_bus_push_inbound()
    v
[Message Bus] ──> [Agent Loop] ──> [Message Bus]
                   (Claude/GPT)         |
                                        |  outbound dispatch
                                        v
                              [feishu_send_message()]
                                        |
                                        |  POST /im/v1/messages
                                        v
                                   Feishu API
```

### Key Components

| Component | Description |
|-----------|-------------|
| **Webhook Server** | HTTP server on port 18790, handles event callbacks |
| **Token Manager** | Manages tenant access tokens, auto-refreshes before expiry |
| **Message Sender** | Sends text messages via Feishu REST API with auto-chunking |
| **Deduplication** | Prevents processing duplicate events from Feishu retries |

### Configuration Constants

These can be found in `main/mimi_config.h`:

| Constant | Default | Description |
|----------|---------|-------------|
| `MIMI_FEISHU_MAX_MSG_LEN` | 4096 | Max message length per chunk |
| `MIMI_FEISHU_WEBHOOK_PORT` | 18790 | Webhook HTTP server port |
| `MIMI_FEISHU_WEBHOOK_PATH` | `/feishu/events` | Webhook endpoint path |
| `MIMI_FEISHU_WEBHOOK_MAX_BODY` | 16 KB | Max webhook request body size |

## CLI Commands

| Command | Description |
|---------|-------------|
| `set_feishu_creds <app_id> <app_secret>` | Save Feishu credentials to NVS |
| `config_show` | Show all configuration (including Feishu, masked) |
| `config_reset` | Clear all NVS config, revert to build-time defaults |

## Troubleshooting

### Webhook URL verification fails

- Ensure the ESP32 is running and connected to WiFi: `wifi_status`
- Verify the webhook URL is reachable from the internet
- Check that port 18790 is not blocked by firewalls
- Look at ESP32 serial output for incoming HTTP requests

### Bot doesn't respond to messages

1. **Check credentials**: `config_show` should show Feishu app_id and app_secret
2. **Check event subscription**: Ensure `im.message.receive_v1` is subscribed in the Feishu app settings
3. **Check permissions**: Both `im:message` and `im:message:send_as_bot` must be granted
4. **Check serial output**: Look for message processing logs on the ESP32

### "Tenant access token" errors

- Verify your App ID and App Secret are correct
- The token auto-refreshes every 2 hours — if you just set credentials, wait a moment for the first token fetch
- Ensure the ESP32 can reach `https://open.feishu.cn` (check proxy settings if needed)

### Messages are truncated

Feishu has a 4096-character limit per message. MimiClaw automatically chunks long messages, but if you see issues, check the serial output for chunking errors.

### Bot works in DM but not in groups

- Ensure the bot is added to the group
- Users must `@mention` the bot in group chats for it to receive messages
- Check that group messaging permissions are enabled in the Feishu app settings

### Event subscription shows errors in Feishu console

- Feishu retries failed events up to 5 times with exponential backoff
- MimiClaw deduplicates retried events, so duplicate processing is not a concern
- If events consistently fail, check the ESP32's network connectivity

## References

- [Feishu Open Platform Documentation](https://open.feishu.cn/document/home/index)
- [Feishu Bot Development Guide](https://open.feishu.cn/document/client-docs/bot-v3/bot-overview)
- [Feishu Message API](https://open.feishu.cn/document/server-docs/im-v1/message/create)
- [Feishu Event Subscription Guide](https://open.feishu.cn/document/server-docs/event-subscription/event-subscription-guide)
- [Lark Developer Documentation](https://open.larksuite.com/document/home/index) (international version)
- [Volcengine OpenClaw Deployment Guide](https://www.volcengine.com/docs/6396/2189942) (Chinese)
