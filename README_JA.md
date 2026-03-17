# reSpeaker-claw: ReSpeaker XVF3800 向け音声 AI Agent

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

reSpeaker-claw は、ReSpeaker XVF3800 ベースのデバイスを音声ファーストの AI Agent に変えるプロジェクトです。I2S で音声を取り込み、ローカル VAD を実行し、発話を STT に送って組み込みの agent loop で処理します。システムはリアルタイム音声対話に加えて、ローカルメモリ、ツール呼び出し、スケジューリング、heartbeat、OTA 更新、プロキシ対応を統合し、最終的に TTS でスピーカーから応答を返します。

## reSpeaker-claw とは

- **小さい**: Linux なし、Node.js なし、無駄な依存なし、純粋な C のみ
- **記憶する**: メモリから学習し、再起動後も文脈を保持
- **省電力**: USB 給電、より低消費電力で 24/7 稼働可能
- **自由度が高い**: ReSpeaker XVF3800 のマイクアレイに、好みのアンプや DAC を組み合わせ可能
- **扱いやすい**: 音声チャネルを内蔵し、XVF3800 とスピーカー経路以外の追加ハードウェアをほぼ必要としない

## 特長

- 音声入力: ReSpeaker XVF3800 マイクアレイを I2S で接続
- 音声出力: TTS 音声のダウンロード、WAV デコード、リサンプル、I2S 再生
- マルチチャネル Agent: 音声、Telegram、Feishu、WebSocket
- ローカル永続化: SPIFFS にメモリ、設定、セッション、cron ジョブ、日次メモを保存
- 互換 LLM バックエンド: 公式 Anthropic / OpenAI API に加え、Anthropic 互換または OpenAI 互換エンドポイントも利用可能
- STT / TTS を柔軟に設定可能: URL、API Key、モデル、音色、言語を自由に差し替え可能
- 実行時オーバーライド: WiFi、provider、model、API base、proxy、token をシリアル CLI から変更可能

## クイックスタート

### 必要なもの

- reSpeaker XVF3800 USB 4 Microphone Array と XIAO ESP32S3 ボード
- I2S 出力で接続するスピーカー / DAC / アンプ経路
- 書き込みとシリアルモニタ用の USB ケーブル
- WiFi 接続
- ESP-IDF v5.5+
- 任意: Telegram を使う場合は Telegram Bot Token
- 任意: Feishu を使う場合は Feishu アプリ認証情報
- Anthropic 互換または OpenAI 互換エンドポイント向けの LLM API Key
- 音声モード用の STT サービスと TTS サービス

### クローンとビルド環境

まず公式ガイドを参照して I2S ファームウェアを書き込んでください:
[SeeedStudio wiki](https://wiki.seeedstudio.com/respeaker_xvf3800_introduction/#flash-firmware)

その後、このプロジェクトをクローンしてターゲットを設定します:

```bash
git clone https://github.com/Seeed-Projects/reSpeaker-claw
cd reSpeaker-claw

idf.py set-target esp32s3
```

ESP-IDF は先にインストールしてください: [ESP-IDF Install](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/get-started/)

Ubuntu 用ヘルパースクリプト:

```bash
./scripts/setup_idf_ubuntu.sh
./scripts/build_ubuntu.sh
```

macOS 用ヘルパースクリプト:

```bash
./scripts/setup_idf_macos.sh
./scripts/build_macos.sh
```

## 設定

まず secrets のサンプルファイルをコピーします:

```bash
cp "main/mimi_secrets.h.example" "main/mimi_secrets.h"
```

`main/mimi_secrets.h` を編集し、実際に使う項目を設定します:

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

補足:

- `MIMI_SECRET_MODEL_PROVIDER` はベンダ名ではなく、リクエストプロトコルを選択します
- OpenAI 互換ゲートウェイには `openai` を使用します
- Anthropic 互換ゲートウェイには `anthropic` を使用します
- 音声モードでは STT と TTS の URL / Key を両方設定する必要があります
- LLM API base は実行時に `set_api_base` で変更できます

## STT と TTS の追加

このプロジェクトでは、音声を後付け機能として扱っていません。完全な ReSpeaker 体験を有効にするには:

1. `MIMI_SECRET_STT_URL`、`MIMI_SECRET_STT_API_KEY`、`MIMI_SECRET_STT_MODEL` を設定します
2. `MIMI_SECRET_TTS_URL`、`MIMI_SECRET_TTS_API_KEY`、`MIMI_SECRET_TTS_MODEL`、`MIMI_SECRET_TTS_VOICE`、`MIMI_SECRET_TTS_LANGUAGE` を設定します
3. I2S セクションで XVF3800 の入力ピンとスピーカー側の出力ピンを設定します
4. DAC やアンプの音がノイズになる場合は、`MIMI_VOICE_I2S_STD_SLOT_STYLE` をハードウェアのタイミングに合わせて設定します
5. 室内環境で誤検知が多い場合は、`MIMI_VOICE_VAD_START_FRAMES`、`MIMI_VOICE_VAD_MIN_FRAMES`、`MIMI_VOICE_STT_COOLDOWN_MS` を調整します
6. TTS 音声が長すぎる場合は、`MIMI_VOICE_TTS_MAX_SECONDS`、`MIMI_VOICE_TTS_CHARS_PER_SEC`、`MIMI_VOICE_TTS_MAX_CHARS` を調整します

現在のファームウェアには、すでに完全な音声チャネルが含まれています:

- 入力方向: mic PCM -> VAD -> STT -> message bus
- 出力方向: agent text -> TTS -> playback

## 書き込みとモニタ

`main/mimi_secrets.h` を変更した後は、クリーンな状態から再ビルドしてください:

```bash
idf.py fullclean
idf.py build
```

シリアルポートを確認します:

```bash
ls /dev/cu.usb*      # macOS
ls /dev/ttyACM*      # Linux
```

書き込みとモニタ:

```bash
idf.py -p PORT flash monitor
```

`PORT` は実際のデバイスパスに置き換えてください。

## シリアル CLI

シリアル CLI は、NVS に保存される実行時設定を最も素早く変更する方法です:

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

メンテナンス用コマンド:

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

## 互換 Provider モデル

`reSpeaker-claw` は公式の Anthropic と OpenAI のエンドポイントだけに限定されません。

対応内容:

- `set_model_provider anthropic` で選択する Anthropic 互換サービス
- `set_model_provider openai` で選択する OpenAI 互換サービス
- `set_api_base` で切り替える任意の API base

これにより、agent loop を変更せずに、ローカルゲートウェイ、地域クラウド、統合 API プラットフォームを利用できます。

## メモリと自動化

Agent は SPIFFS 上に状態をプレーンテキストファイルとして保存します:

| ファイル | 用途 |
|----------|------|
| `SOUL.md` | アシスタント人格 |
| `USER.md` | ユーザープロファイル |
| `MEMORY.md` | 長期記憶 |
| `HEARTBEAT.md` | 定期実行する自律タスクリスト |
| `cron.json` | スケジュールジョブ |
| `tg_12345.jsonl` | セッション履歴 |

組み込みの自動化機能:

- `cron_add`、`cron_list`、`cron_remove`
- heartbeat 駆動の能動的タスク処理
- ReAct loop におけるツール呼び出し
- 再起動後も保持されるローカル状態

## ツール

組み込みツール:

- `web_search`
- `get_current_time`
- `cron_add`
- `cron_list`
- `cron_remove`
- Agent ランタイムが使う SPIFFS ファイル操作ツール

Web 検索を有効にするには、次のいずれかを設定します:

- `MIMI_SECRET_TAVILY_KEY`
- `MIMI_SECRET_SEARCH_KEY`

## 謝辞

本プロジェクトは元の [mimiclaw](https://github.com/memovai/mimiclaw) を基盤としています。reSpeaker-claw は、その組み込み agent 基盤を ReSpeaker XVF3800 の音声ハードウェア向けに適応し、STT / TTS パイプラインを拡張しつつ、マルチチャネル agent アーキテクチャを継承しています。

## ライセンス

MIT
