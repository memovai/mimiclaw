# reSpeaker-claw: Voice AI Agent for ReSpeaker XVF3800

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-yellow.svg" alt="License: MIT"></a>
  <img src="https://img.shields.io/badge/language-C-00599C.svg" alt="Language: C">
  <img src="https://img.shields.io/badge/framework-ESP--IDF%20v5.5%2B-E7352C.svg" alt="Framework: ESP-IDF v5.5+">
  <img src="https://img.shields.io/badge/hardware-ReSpeaker%20XVF3800-1F6FEB.svg" alt="Hardware: ReSpeaker XVF3800">
  <img src="https://img.shields.io/badge/architecture-Voice%20Agent-0A7E3B.svg" alt="Architecture: Voice Agent">
</p>

<p align="center">
  <strong><a href="README.md">English</a> | <a href="README_CN.md">中文</a> | <a href="README_JA.md">日本語</a></strong>
</p>

reSpeaker-claw turns a ReSpeaker XVF3800–based device into a voice-first AI agent. It captures audio over I2S, performs local VAD, sends utterances to STT, and processes them through an embedded agent loop. The system combines real-time speech interaction with local memory, tool calling, scheduling, heartbeat processes, OTA updates, and proxy support, and returns responses via TTS through the speaker.

## Meet reSpeaker-claw

- **Tiny** — No Linux, no Node.js, no bloat — just pure C
- **Loyal** — Learns from memory, remembers across reboots
- **Energetic** — USB power, lower power consumption, runs 24/7
- **Freedom** — ReSpeaker XVF3800's mic array + your choice of speaker amp/DAC
- **Handy** — Built-in voice channel, no extra hardware needed beyond the XVF3800 and a speaker path

## Highlights

- Voice input: ReSpeaker XVF3800 microphone array over I2S
- Voice output: TTS audio download, WAV decode, resample, and speaker playback over I2S
- Multi-channel agent: voice, Telegram, Feishu, WebSocket
- Local persistence: SPIFFS stores memory, profiles, sessions, cron jobs, and daily notes
- Compatible LLM backends: official Anthropic/OpenAI APIs or third-party gateways that expose Anthropic-compatible or OpenAI-compatible endpoints
- Configurable STT/TTS: plug in your own service URL, API key, model, voice, and language
- Runtime overrides: change WiFi, provider, model, API base, proxy, and tokens from the serial CLI without editing code


## Quick Start

### Requirements

- A reSpeaker XVF3800 USB 4 Microphone Array with XIAO ESP32S3 board
- A speaker / DAC / amp path on I2S output
- A USB cable for flashing and serial monitoring
- WiFi access
- ESP-IDF v5.5+
- Optional: Telegram bot token if you want Telegram
- Optional: Feishu app credentials if you want Feishu
- One LLM API key for an Anthropic-compatible or OpenAI-compatible endpoint
- One STT service and one TTS service for voice mode

### Clone and Build Environment

Refer to the official guide to flash the I2S firmware:
[SeeedStudio wiki](https://wiki.seeedstudio.com/respeaker_xvf3800_introduction/#flash-firmware)

Then clone this project and set the target:

```bash
git clone https://github.com/Seeed-Projects/reSpeaker-claw
cd reSpeaker-claw

idf.py set-target esp32s3
```

Install ESP-IDF first:  [ESP-IDF Install](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/get-started/)

Ubuntu helper scripts:

```bash
./scripts/setup_idf_ubuntu.sh
./scripts/build_ubuntu.sh
```

macOS helper scripts:

```bash
./scripts/setup_idf_macos.sh
./scripts/build_macos.sh
```

## Configure

Copy the example secrets file:

```bash
cp "main/mimi_secrets.h.example" "main/mimi_secrets.h"
```

Edit `main/mimi_secrets.h` and set the fields you actually use:

```c
/* WiFi */
#define MIMI_SECRET_WIFI_SSID       "YourWiFiName"
#define MIMI_SECRET_WIFI_PASS       "YourWiFiPassword"

/* Optional text channels */
#define MIMI_SECRET_TG_TOKEN        ""
#define MIMI_SECRET_FEISHU_APP_ID   ""
#define MIMI_SECRET_FEISHU_APP_SECRET ""

/* LLM */
#define MIMI_SECRET_API_KEY         "your-llm-key"
#define MIMI_SECRET_MODEL           "your-model"
#define MIMI_SECRET_MODEL_PROVIDER  "openai"      /* or "anthropic" */

/* Search and proxy */
#define MIMI_SECRET_TAVILY_KEY      ""
#define MIMI_SECRET_SEARCH_KEY      ""
#define MIMI_SECRET_PROXY_HOST      ""
#define MIMI_SECRET_PROXY_PORT      ""
#define MIMI_SECRET_PROXY_TYPE      ""            /* "http" or "socks5" */

/* Voice STT / TTS */
#define MIMI_SECRET_STT_URL         "https://your-stt-endpoint"
#define MIMI_SECRET_STT_API_KEY     "your-stt-key"
#define MIMI_SECRET_STT_MODEL       "your-stt-model"
#define MIMI_SECRET_TTS_URL         "https://your-tts-endpoint"
#define MIMI_SECRET_TTS_API_KEY     "your-tts-key"
#define MIMI_SECRET_TTS_MODEL       "your-tts-model"
#define MIMI_SECRET_TTS_VOICE       ""
#define MIMI_SECRET_TTS_LANGUAGE    "English"

/* ReSpeaker XVF3800 I2S pin map */
#define MIMI_VOICE_I2S_PORT         0
#define MIMI_VOICE_I2S_BCLK         GPIO_NUM_8
#define MIMI_VOICE_I2S_WS           GPIO_NUM_7
#define MIMI_VOICE_I2S_DIN          GPIO_NUM_43
#define MIMI_VOICE_I2S_DOUT         GPIO_NUM_44
```

Notes:

- `MIMI_SECRET_MODEL_PROVIDER` selects the request protocol, not just the vendor name.
- Use `openai` for OpenAI-compatible gateways.
- Use `anthropic` for Anthropic-compatible gateways.
- Voice mode requires STT and TTS URL/key pairs to be configured.
- LLM API base can be changed at runtime with `set_api_base`.

## Adding STT and TTS

This project no longer treats speech as an afterthought. To enable the full ReSpeaker experience:

1. Configure `MIMI_SECRET_STT_URL`, `MIMI_SECRET_STT_API_KEY`, and `MIMI_SECRET_STT_MODEL`.
2. Configure `MIMI_SECRET_TTS_URL`, `MIMI_SECRET_TTS_API_KEY`, `MIMI_SECRET_TTS_MODEL`, `MIMI_SECRET_TTS_VOICE`, and `MIMI_SECRET_TTS_LANGUAGE`.
3. Set the XVF3800 input pins and your speaker output pins in the I2S section.
4. If your DAC or amp sounds noisy, set `MIMI_VOICE_I2S_STD_SLOT_STYLE` to match the hardware timing style.
5. If your room causes false triggers, tune `MIMI_VOICE_VAD_START_FRAMES`, `MIMI_VOICE_VAD_MIN_FRAMES`, and `MIMI_VOICE_STT_COOLDOWN_MS`.
6. If your TTS audio is too long, tune `MIMI_VOICE_TTS_MAX_SECONDS`, `MIMI_VOICE_TTS_CHARS_PER_SEC`, and `MIMI_VOICE_TTS_MAX_CHARS`.

The current firmware already contains the full voice channel:

- inbound: mic PCM -> VAD -> STT -> message bus
- outbound: agent text -> TTS -> playback

## Flash and Monitor

After changing `main/mimi_secrets.h`, rebuild from a clean state:

```bash
idf.py fullclean
idf.py build
```

Find your serial port:

```bash
ls /dev/cu.usb*      # macOS
ls /dev/ttyACM*      # Linux
```

Flash and monitor:

```bash
idf.py -p PORT flash monitor
```

Replace `PORT` with your actual device path.

## Serial CLI

The serial CLI is the fastest way to change runtime settings stored in NVS:

```
mimi> wifi_set MySSID MyPassword
mimi> set_tg_token 123456:ABC...
mimi> set_api_key your-llm-key
mimi> set_api_base https://your-compatible-endpoint/v1
mimi> set_model_provider openai
mimi> set_model gpt-5.2
mimi> set_proxy 127.0.0.1 7897
mimi> clear_proxy
mimi> set_search_key BSA...
mimi> set_tavily_key tvly-...
mimi> config_show
mimi> config_reset
```

Maintenance commands:

```text
mimi> wifi_status
mimi> memory_read
mimi> memory_write "remember this"
mimi> heap_info
mimi> session_list
mimi> session_clear 12345
mimi> heartbeat_trigger
mimi> cron_start
mimi> restart
```

## Compatible Provider Model

`reSpeaker-claw` is not limited to the official Anthropic and OpenAI endpoints.

It supports:

- Anthropic protocol compatible services, selected with `set_model_provider anthropic`
- OpenAI protocol compatible services, selected with `set_model_provider openai`
- Custom API bases through `set_api_base`

This makes it practical to use local gateways, regional cloud vendors, or unified API platforms without changing the agent loop.

## Memory and Automation

The agent persists its state in plain files on SPIFFS:

| File | Purpose |
|------|---------|
| `SOUL.md` | Assistant persona |
| `USER.md` | User profile |
| `MEMORY.md` | Long-term memory |
| `HEARTBEAT.md` | Periodic autonomous task list |
| `cron.json` | Scheduled jobs |
| `tg_12345.jsonl` | Session history |

Built-in automation features:

- `cron_add`, `cron_list`, `cron_remove`
- heartbeat-driven proactive task handling
- tool calling in the ReAct loop
- local storage that survives reboot

## Tooling

Built-in tools include:

- `web_search`
- `get_current_time`
- `cron_add`
- `cron_list`
- `cron_remove`
- SPIFFS file tools used by the agent runtime

For web search, configure either:

- `MIMI_SECRET_TAVILY_KEY`
- `MIMI_SECRET_SEARCH_KEY`

## Acknowledgments

This project builds on the original [mimiclaw](https://github.com/memovai/mimiclaw). reSpeaker-claw adapts that embedded agent foundation to ReSpeaker XVF3800 voice hardware, extends the STT / TTS pipeline, and continues the multi-channel agent architecture.

## License

MIT
