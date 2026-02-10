# MimiClaw: $5 芯片上的口袋 AI 助理

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![DeepWiki](https://img.shields.io/badge/DeepWiki-mimiclaw-blue.svg)](https://deepwiki.com/memovai/mimiclaw)
[![Discord](https://img.shields.io/badge/Discord-mimiclaw-5865F2?logo=discord&logoColor=white)](https://discord.gg/r8ZxSvB8Yr)
[![X](https://img.shields.io/badge/X-@ssslvky-black?logo=x)](https://x.com/ssslvky)

**[English](README.md) | [中文](README_CN.md)**

<p align="center">
  <img src="assets/banner.png" alt="MimiClaw" width="480" />
</p>

**$5 芯片上的 AI 助理（OpenClaw）。没有 Linux，没有 Node.js，纯 C。**

MimiClaw 把一块小小的 ESP32-S3 开发板变成你的私人 AI 助理。插上 USB 供电，连上 WiFi，通过 Telegram 跟它对话 — 它能处理你丢给它的任何任务，还会随时间积累本地记忆不断进化 — 全部跑在一颗拇指大小的芯片上。

## 认识 MimiClaw

- **小巧** — 没有 Linux，没有 Node.js，没有臃肿依赖 — 纯 C
- **好用** — 在 Telegram 发消息，剩下的它来搞定
- **忠诚** — 从记忆中学习，跨重启也不会忘
- **能干** — USB 供电，0.5W，24/7 运行
- **可爱** — 一块 ESP32-S3 开发板，$5，没了

## 工作原理

![](assets/mimiclaw.png)

你在 Telegram 发一条消息，ESP32-S3 通过 WiFi 收到后送进 Agent 循环 — Claude 思考、调用工具、读取记忆 — 再把回复发回来。一切都跑在一颗 $5 的芯片上，所有数据存在本地 Flash。

## 快速开始

### 你需要

- 一块 **ESP32-S3 开发板**，16MB Flash + 8MB PSRAM（如小智 AI 开发板，~¥30）
- 一根 **USB Type-C 数据线**
- 一个 **Telegram Bot Token** — 在 Telegram 找 [@BotFather](https://t.me/BotFather) 创建
- 一个 **Anthropic API Key** — 从 [console.anthropic.com](https://console.anthropic.com) 获取

### 安装

```bash
# 需要先安装 ESP-IDF v5.5+:
# https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/

git clone https://github.com/memovai/mimiclaw.git
cd mimiclaw

idf.py set-target esp32s3
```

### 配置

MimiClaw 使用**三层配置**：
- `idf.py menuconfig` 编译时默认值（推荐用于 LLM base_url 和 API key）
- `mimi_secrets.h` 可选兜底默认值
- 串口 CLI 运行时覆盖（存入 NVS，优先级最高）

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

先在 menuconfig 里配置 LLM：

```bash
idf.py menuconfig
# MimiClaw -> LLM Base URL
# MimiClaw -> LLM API Key
```

可选：编辑 `main/mimi_secrets.h` 作为兜底：

```c
#define MIMI_SECRET_WIFI_SSID       "你的WiFi名"
#define MIMI_SECRET_WIFI_PASS       "你的WiFi密码"
#define MIMI_SECRET_TG_TOKEN        "123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11"
#define MIMI_SECRET_API_KEY         ""              // 可选兜底：仅当 menuconfig 未配置时使用
#define MIMI_SECRET_LLM_BASE_URL    ""              // 可选：覆盖/兜底 menuconfig 的 base_url
#define MIMI_SECRET_SEARCH_KEY      ""              // 可选：Brave Search API key
#define MIMI_SECRET_PROXY_HOST      "10.0.0.1"      // 可选：代理地址
#define MIMI_SECRET_PROXY_PORT      "7897"           // 可选：代理端口
```

然后编译烧录：

```bash
# 完整编译（修改 mimi_secrets.h 后必须 fullclean）
idf.py fullclean && idf.py build

# 查找串口
ls /dev/cu.usb*          # macOS
ls /dev/ttyACM*          # Linux

# 烧录并监控（将 PORT 替换为你的串口）
# USB 转接器：大概率是 /dev/cu.usbmodem11401（macOS）或 /dev/ttyACM0（Linux）
idf.py -p PORT flash monitor
```

### AP 配网（无需硬编码 WiFi）

当没有 WiFi 凭据，或 STA 连接超时时，MimiClaw 会自动启动配网 AP：

- AP 名称：`MimiClaw-Setup`
- 配网页地址：`http://192.168.4.1`
- 流程：填写参数 -> 保存 -> 自动重启

配网页面带有美化 UI，并提供附近 WiFi 扫描下拉列表，手机和桌面都可直接使用。
同时支持一页配置：
- WiFi SSID/密码
- Telegram Bot Token
- LLM Base URL / API Key / Model
- Search API Key
- 可选 HTTP 代理 Host/Port

### 代理配置（国内用户）

在国内需要代理才能访问 Telegram 和 Anthropic API。MimiClaw 内置 HTTP CONNECT 隧道支持。

**前提**：局域网内有一个支持 HTTP CONNECT 的代理（Clash Verge、V2Ray 等），并开启了「允许局域网连接」。

可以在 `mimi_secrets.h` 中编译时设置，也可以通过串口 CLI 随时修改：

```
mimi> set_proxy 192.168.1.83 7897   # 设置代理
mimi> clear_proxy                    # 清除代理
```

> **提示**：确保 ESP32-S3 和代理机器在同一局域网。Clash Verge 在「设置 → 允许局域网」中开启。

### CLI 命令

通过串口连接即可配置和调试。**配置命令**让你无需重新编译就能修改设置 — 随时随地插上 USB 线就能改。

**运行时配置**（存入 NVS，覆盖编译时默认值）：

```
mimi> wifi_set MySSID MyPassword   # 换 WiFi
mimi> set_tg_token 123456:ABC...   # 换 Telegram Bot Token
mimi> set_api_key sk-ant-api03-... # 换 Anthropic API Key
mimi> set_model claude-sonnet-4-5-20250929  # 换模型
mimi> set_base_url https://api.anthropic.com/v1/messages  # 换 LLM endpoint
mimi> set_proxy 192.168.1.83 7897  # 设置代理
mimi> clear_proxy                  # 清除代理
mimi> set_search_key BSA...        # 设置 Brave Search API Key
mimi> config_show                  # 查看所有配置（脱敏显示）
mimi> config_reset                 # 清除 NVS，恢复编译时默认值
```

**调试与运维：**

```
mimi> wifi_status              # 连上了吗？
mimi> memory_read              # 看看它记住了什么
mimi> memory_write "内容"       # 写入 MEMORY.md
mimi> heap_info                # 还剩多少内存？
mimi> session_list             # 列出所有会话
mimi> session_clear 12345      # 删除一个会话
mimi> restart                  # 重启
```

## 记忆

MimiClaw 把所有数据存为纯文本文件，可以直接读取和编辑：

| 文件 | 说明 |
|------|------|
| `SOUL.md` | 机器人的人设 — 编辑它来改变行为方式 |
| `USER.md` | 关于你的信息 — 姓名、偏好、语言 |
| `MEMORY.md` | 长期记忆 — 它应该一直记住的事 |
| `2026-02-05.md` | 每日笔记 — 今天发生了什么 |
| `tg_12345.jsonl` | 聊天记录 — 你和它的对话 |

## 工具

MimiClaw 使用 Anthropic 的 tool use 协议 — Claude 在对话中可以调用工具，循环执行直到任务完成（ReAct 模式）。

| 工具 | 说明 |
|------|------|
| `web_search` | 通过 Brave Search API 搜索网页，获取实时信息 |
| `get_current_time` | 通过 HTTP 获取当前日期和时间，并设置系统时钟 |

启用网页搜索需要在 `mimi_secrets.h` 中设置 [Brave Search API key](https://brave.com/search/api/)（`MIMI_SECRET_SEARCH_KEY`）。

## 其他功能

- **WebSocket 网关** — 端口 18789，局域网内用任意 WebSocket 客户端连接
- **OTA 更新** — WiFi 远程刷固件，无需 USB
- **双核** — 网络 I/O 和 AI 处理分别跑在不同 CPU 核心
- **HTTP 代理** — CONNECT 隧道，适配受限网络
- **工具调用** — ReAct Agent 循环，Anthropic tool use 协议

## 开发者

技术细节在 `docs/` 文件夹：

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — 系统设计、模块划分、任务布局、内存分配、协议、Flash 分区
- **[docs/TODO.md](docs/TODO.md)** — 功能差距和路线图

## 许可证

MIT

## 致谢

灵感来自 [OpenClaw](https://github.com/openclaw/openclaw) 和 [Nanobot](https://github.com/HKUDS/nanobot)。MimiClaw 为嵌入式硬件重新实现了核心 AI Agent 架构 — 没有 Linux，没有服务器，只有一颗 $5 的芯片。

## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=memovai/mimiclaw&type=Date)](https://star-history.com/#memovai/mimiclaw&Date)
