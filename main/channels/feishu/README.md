# Feishu/Lark Bot Integration

This directory contains the Feishu (飞书) / Lark bot integration for MimiClaw.

## Features

- ✅ Send text messages to Feishu chats
- ✅ Automatic message chunking (4096 chars per message)
- ✅ Tenant access token management with auto-refresh
- ⚠️ Event subscription support (webhook/websocket required for receiving messages)

## Configuration

### Option 1: Build-time Configuration (Recommended)

1. Copy the secrets template:
```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

2. Edit `main/mimi_secrets.h` and add your Feishu app credentials:
```c
#define MIMI_SECRET_FEISHU_APP_ID     "cli_xxxxxxxxxxxxxx"
#define MIMI_SECRET_FEISHU_APP_SECRET "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
```

3. Rebuild the project:
```bash
idf.py fullclean && idf.py build
```

### Option 2: Runtime Configuration (CLI)

After flashing, you can configure credentials via serial CLI:

```bash
mimi> set_feishu_creds cli_xxxxxxxxxxxxxx xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
Feishu credentials saved.
```

## Getting Feishu Credentials

1. Go to [Feishu Open Platform](https://open.feishu.cn/) (or [Lark Open Platform](https://open.larksuite.com/))
2. Create a new app or select an existing one
3. Get your **App ID** and **App Secret** from the app credentials page
4. Enable the following permissions:
   - `im:message` - Send and receive messages
   - `im:message:send_as_bot` - Send messages as bot
   - `contact:user.base:readonly` - Read user info (optional, for sender name resolution)

## Architecture

### Sending Messages

The bot uses the Feishu REST API with tenant access token authentication:

1. **Token Management**: Automatically fetches and caches tenant access token
2. **Message API**: Uses `POST /open-apis/im/v1/messages` to send messages
3. **Chunking**: Splits messages > 4096 characters into multiple messages

### Receiving Messages (Not Yet Implemented)

Feishu uses event subscription model, not polling like Telegram. To receive messages, you need to:

1. Set up a webhook endpoint (HTTP server)
2. Configure the event subscription URL in Feishu admin console
3. Handle incoming event callbacks

**Current Status**: The polling task is a placeholder. Full implementation requires:
- Webhook HTTP server (similar to WebSocket gateway)
- Event signature verification
- Message event parsing and routing to message bus

## Message Bus Integration

The Feishu bot integrates with MimiClaw's message bus:

- **Outbound**: Listens for `MIMI_CHAN_FEISHU` messages and sends them via Feishu API
- **Inbound**: (Not yet implemented) Will push received messages to the message bus for agent processing

## API Reference

### Functions

#### `esp_err_t feishu_bot_init(void)`
Initialize the Feishu bot and load credentials from NVS.

#### `esp_err_t feishu_bot_start(void)`
Start the Feishu polling task (currently a placeholder).

#### `esp_err_t feishu_send_message(const char *chat_id, const char *text)`
Send a text message to a Feishu chat.

- **Parameters**:
  - `chat_id`: Feishu chat ID (e.g., "oc_xxxxxxxxxxxxxxxxx")
  - `text`: Message text (will be chunked if > 4096 chars)

#### `esp_err_t feishu_set_credentials(const char *app_id, const char *app_secret)`
Save Feishu app credentials to NVS.

## Differences from Telegram Integration

| Feature | Telegram | Feishu |
|---------|----------|--------|
| Auth Method | Bot Token | App ID + App Secret → Tenant Token |
| Message Receiving | Long Polling | Event Subscription (Webhook) |
| Message Sending | Direct API | REST API with Token |
| Token Refresh | N/A | Auto-refresh (7200s expiry) |

## TODO

- [ ] Implement webhook server for receiving messages
- [ ] Add event signature verification
- [ ] Support rich message types (cards, images, files)
- [ ] Support group chat mentions
- [ ] Add chat ID resolution helpers
- [ ] Implement websocket connection mode (alternative to webhook)

## References

- [Feishu Open Platform Docs](https://open.feishu.cn/document/home/index)
- [Message API Reference](https://open.feishu.cn/document/server-docs/im-v1/message/create)
- [Event Subscription Guide](https://open.feishu.cn/document/server-docs/event-subscription/event-subscription-guide)
