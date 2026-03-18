# Feishu/Lark Bot Integration

This directory contains the Feishu bot integration for MimiClaw.

## Features

- Send text messages to Feishu chats
- Receive messages via WebSocket persistent connection (long-connection mode)
- Automatic message chunking (4096 chars per message)
- Tenant access token management with auto-refresh
- Message deduplication
- Reply to specific messages
- Support for both DM (p2p) and group chats

## Configuration

### Option 1: Build-time Configuration

1. Copy the secrets template:
```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

2. Edit `main/mimi_secrets.h`:
```c
#define MIMI_SECRET_FEISHU_APP_ID     "cli_xxxxxxxxxxxxxx"
#define MIMI_SECRET_FEISHU_APP_SECRET "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
```

3. Rebuild:
```bash
idf.py fullclean && idf.py build
```

### Option 2: Runtime Configuration (CLI)

```
mimi> set_feishu_creds cli_xxxxxxxxxxxxxx xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
```

## Feishu App Setup

1. Go to [Feishu Open Platform](https://open.feishu.cn/)
2. Create an app and get **App ID** / **App Secret**
3. Enable permissions:
   - `im:message` - Send and receive messages
   - `im:message:send_as_bot` - Send messages as bot
4. Configure Event Subscription:
   - Set subscription mode to **Persistent connection** (长连接)
   - Subscribe to: `im.message.receive_v1`
5. The ESP32 will connect to Feishu automatically on boot

## Architecture

```
Feishu Server (wss://open.feishu.cn)
    ^
    |  (WebSocket persistent connection, ESP32 initiates)
[feishu_ws_task]
    |
    v  (message_bus_push_inbound)
[Message Bus] --> [Agent Loop] --> [Message Bus]
    |                                    |
    v  (outbound dispatch)               |
[feishu_send_message] <-----------------+
    |
    v  (POST /im/v1/messages)
Feishu API
```

## API Reference

| Function | Description |
|----------|-------------|
| `feishu_bot_init()` | Load credentials from NVS/build-time |
| `feishu_bot_start()` | Start WebSocket persistent connection task |
| `feishu_send_message(chat_id, text)` | Send text message |
| `feishu_reply_message(message_id, text)` | Reply to a specific message |
| `feishu_set_credentials(app_id, secret)` | Save credentials to NVS |

## References

- [Feishu Open Platform Docs](https://open.feishu.cn/document/home/index)
- [Message API](https://open.feishu.cn/document/server-docs/im-v1/message/create)
- [Long-connection Event Subscription](https://open.feishu.cn/document/server-docs/event-subscription/long-connection)
