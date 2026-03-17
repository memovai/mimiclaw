# reSpeaker-claw：面向 ReSpeaker XVF3800 的语音 AI Agent

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

reSpeaker-claw 将基于 ReSpeaker XVF3800 的设备变成一个以语音为主入口的 AI Agent。它通过 I2S 采集音频，在本地执行 VAD，将话语送入 STT，并通过嵌入式 agent loop 处理。系统把实时语音交互、本地记忆、工具调用、调度、heartbeat、OTA 更新和代理支持整合在一起，最后通过 TTS 从扬声器返回响应。

## 认识 reSpeaker-claw

- **小巧**：没有 Linux，没有 Node.js，没有臃肿依赖，只有纯 C
- **忠诚**：从记忆中学习，重启后依然保留上下文
- **高效**：USB 供电，功耗更低，可 24/7 运行
- **自由**：ReSpeaker XVF3800 麦克风阵列，配合你自己选择的功放或 DAC
- **顺手**：内置语音通道，除了 XVF3800 和扬声器链路，不需要额外硬件

## 亮点

- 语音输入：ReSpeaker XVF3800 麦克风阵列，通过 I2S 接入
- 语音输出：TTS 音频下载、WAV 解码、重采样与 I2S 播放
- 多通道 Agent：语音、Telegram、飞书、WebSocket
- 本地持久化：SPIFFS 保存记忆、配置、会话、cron 任务和每日笔记
- 兼容 LLM 后端：支持官方 Anthropic / OpenAI API，也支持兼容 Anthropic 或 OpenAI 协议的第三方网关
- 可配置 STT / TTS：可接入你自己的服务 URL、API Key、模型、音色和语言
- 运行时覆盖：可通过串口 CLI 修改 WiFi、provider、model、API base、代理和 token，无需改代码

## 快速开始

### 依赖条件

- 一套 reSpeaker XVF3800 USB 4 Microphone Array 搭配 XIAO ESP32S3 开发板
- 一路 I2S 输出到扬声器 / DAC / 功放
- 一根用于烧录和串口监控的 USB 线
- 可用的 WiFi
- ESP-IDF v5.5+
- 可选：如果你要使用 Telegram，需要 Telegram Bot Token
- 可选：如果你要使用飞书，需要飞书应用凭证
- 一个兼容 Anthropic 或 OpenAI 协议的 LLM API Key
- 一套用于语音模式的 STT 服务和 TTS 服务

### 克隆与构建环境

先参考官方指南刷入 I2S 固件：
[SeeedStudio wiki](https://wiki.seeedstudio.com/respeaker_xvf3800_introduction/#flash-firmware)

然后克隆本项目并设置目标：

```bash
git clone https://github.com/Seeed-Projects/reSpeaker-claw
cd reSpeaker-claw

idf.py set-target esp32s3
```

先安装 ESP-IDF：[ESP-IDF 安装](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/get-started/)

Ubuntu 辅助脚本：

```bash
./scripts/setup_idf_ubuntu.sh
./scripts/build_ubuntu.sh
```

macOS 辅助脚本：

```bash
./scripts/setup_idf_macos.sh
./scripts/build_macos.sh
```

## 配置

复制示例 secrets 文件：

```bash
cp "main/mimi_secrets.h.example" "main/mimi_secrets.h"
```

编辑 `main/mimi_secrets.h`，填写你实际需要的配置项：

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

说明：

- `MIMI_SECRET_MODEL_PROVIDER` 选择的是请求协议，而不只是厂商名
- 兼容 OpenAI 协议的网关使用 `openai`
- 兼容 Anthropic 协议的网关使用 `anthropic`
- 语音模式要求 STT 与 TTS 的 URL / Key 成对配置
- LLM API base 可在运行时通过 `set_api_base` 修改

## 添加 STT 和 TTS

这个项目不再把语音当成附属功能。要启用完整的 ReSpeaker 体验：

1. 配置 `MIMI_SECRET_STT_URL`、`MIMI_SECRET_STT_API_KEY` 和 `MIMI_SECRET_STT_MODEL`
2. 配置 `MIMI_SECRET_TTS_URL`、`MIMI_SECRET_TTS_API_KEY`、`MIMI_SECRET_TTS_MODEL`、`MIMI_SECRET_TTS_VOICE` 和 `MIMI_SECRET_TTS_LANGUAGE`
3. 在 I2S 配置段中设置 XVF3800 的输入引脚和扬声器输出引脚
4. 如果 DAC 或功放播放出来像噪音，设置 `MIMI_VOICE_I2S_STD_SLOT_STYLE` 以匹配硬件时序
5. 如果房间环境导致误触发，调节 `MIMI_VOICE_VAD_START_FRAMES`、`MIMI_VOICE_VAD_MIN_FRAMES` 和 `MIMI_VOICE_STT_COOLDOWN_MS`
6. 如果 TTS 音频过长，调节 `MIMI_VOICE_TTS_MAX_SECONDS`、`MIMI_VOICE_TTS_CHARS_PER_SEC` 和 `MIMI_VOICE_TTS_MAX_CHARS`

当前固件已经包含完整的语音通道：

- 输入方向：mic PCM -> VAD -> STT -> message bus
- 输出方向：agent text -> TTS -> playback

## 烧录与监控

修改 `main/mimi_secrets.h` 后，建议从干净状态重新构建：

```bash
idf.py fullclean
idf.py build
```

查找串口：

```bash
ls /dev/cu.usb*      # macOS
ls /dev/ttyACM*      # Linux
```

烧录并监控：

```bash
idf.py -p PORT flash monitor
```

将 `PORT` 替换为你的实际设备路径。

## 串口 CLI

串口 CLI 是修改 NVS 运行时配置的最快方式：

```text
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

维护命令：

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

## 兼容 Provider 模型

`reSpeaker-claw` 不局限于官方 Anthropic 和 OpenAI 端点。

它支持：

- 兼容 Anthropic 协议的服务，通过 `set_model_provider anthropic` 选择
- 兼容 OpenAI 协议的服务，通过 `set_model_provider openai` 选择
- 通过 `set_api_base` 指向任意兼容 API base

这让你可以在不修改 agent loop 的情况下，直接使用本地网关、区域云厂商或统一 API 平台。

## 记忆与自动化

Agent 会将状态以纯文本文件形式持久化到 SPIFFS：

| 文件 | 用途 |
|------|------|
| `SOUL.md` | 助手人格 |
| `USER.md` | 用户资料 |
| `MEMORY.md` | 长期记忆 |
| `HEARTBEAT.md` | 周期性自主任务列表 |
| `cron.json` | 调度任务 |
| `tg_12345.jsonl` | 会话历史 |

内置自动化能力：

- `cron_add`、`cron_list`、`cron_remove`
- heartbeat 驱动的主动任务处理
- ReAct loop 中的工具调用
- 重启后仍可保留的本地状态

## 工具

内置工具包括：

- `web_search`
- `get_current_time`
- `cron_add`
- `cron_list`
- `cron_remove`
- Agent 运行时使用的 SPIFFS 文件工具

如需启用网页搜索，配置以下任一项：

- `MIMI_SECRET_TAVILY_KEY`
- `MIMI_SECRET_SEARCH_KEY`

## 致谢

本项目基于原始的 [mimiclaw](https://github.com/memovai/mimiclaw)。reSpeaker-claw 将那套嵌入式 agent 基础适配到 ReSpeaker XVF3800 语音硬件之上，扩展了 STT / TTS 流程，并延续了多通道 agent 架构。

## 许可证

MIT
