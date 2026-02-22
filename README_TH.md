# MimiClaw: ผู้ช่วย AI พกพาบนชิปราคา $5

<p align="center">
  <img src="assets/banner.png" alt="MimiClaw" width="500" />
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-yellow.svg" alt="License: MIT"></a>
  <a href="https://deepwiki.com/memovai/mimiclaw"><img src="https://img.shields.io/badge/DeepWiki-mimiclaw-blue.svg" alt="DeepWiki"></a>
  <a href="https://discord.gg/r8ZxSvB8Yr"><img src="https://img.shields.io/badge/Discord-mimiclaw-5865F2?logo=discord&logoColor=white" alt="Discord"></a>
  <a href="https://x.com/ssslvky"><img src="https://img.shields.io/badge/X-@ssslvky-black?logo=x" alt="X"></a>
</p>

<p align="center">
  <strong><a href="README.md">English</a> | <a href="README_CN.md">中文</a> | <a href="README_JA.md">日本語</a> | <a href="README_TH.md">ไทย</a></strong>
</p>

MimiClaw คือโปรเจกต์ที่เปลี่ยนบอร์ด ESP32-S3 ตัวเล็กๆ ให้กลายเป็นผู้ช่วย AI ส่วนตัวของคุณ ทั้งหมดรันอยู่บนชิปราคาไม่กี่ดอลลาร์ ไม่มี Linux ไม่มี Node.js มีแค่ C ล้วนๆ

คุณเพียงแค่:

- ต่อบอร์ดเข้ากับไฟเลี้ยงผ่าน USB
- ให้มันเชื่อมต่อ WiFi
- คุยกับมันผ่าน Telegram

มันจะใช้ LLM คิด วิเคราะห์ เรียกใช้เครื่องมือ อ่าน/เขียนความจำ และตอบกลับคุณโดยอัตโนมัติ ข้อมูลทั้งหมดถูกเก็บอยู่ใน Flash บนบอร์ดเอง

รองรับผู้ให้บริการ LLM หลายราย:

- Anthropic (Claude)
- OpenAI (GPT)
- OpenRouter (API รวมหลายโมเดล เช่น Llama, Claude, GPT ฯลฯ)
- Gemini (ผ่าน OpenAI-compatible endpoint)

คุณสามารถสลับผู้ให้บริการได้ขณะรันงาน โดยไม่ต้องแฟลชเฟิร์มแวร์ใหม่

---

## คุณสมบัติหลัก

- เล็กมาก — ไม่มี Linux, ไม่มี Node.js, ไม่มี dependency หนักๆ
- ใช้ง่าย — คุยผ่าน Telegram เหมือนแชตกับบอทปกติ
- จำได้ — มีไฟล์ความจำบน Flash เก็บสภาพและประสบการณ์
- รันยาว — ใช้ไฟ 0.5 W เปิดทิ้งได้ทั้งวัน
- ยืดหยุ่น — เปลี่ยนผู้ให้บริการ LLM และโมเดลได้เองทีหลัง

---

## ภาพรวมการทำงาน

![](assets/mimiclaw.png)

ลูปหลักของระบบเป็น Agent แบบ ReAct:

1. Telegram รับข้อความจากคุณ
2. MimiClaw แปลงเป็นข้อความเข้า LLM
3. LLM:
   - อ่าน system prompt + memory
   - ตัดสินใจว่าจะตอบเลย หรือเรียก tool (เช่น web_search, cron_add)
4. ถ้ามีการเรียก tool:
   - รันโค้ด C ที่เกี่ยวข้อง
   - ส่งผลลัพธ์กลับเข้า LLM
5. เมื่อได้คำตอบสุดท้าย ระบบส่งกลับไปที่ Telegram

ทุกอย่างเกิดบน ESP32-S3 ไม่มีเซิร์ฟเวอร์เสริม

---

## สิ่งที่ต้องเตรียม

- บอร์ด **ESP32-S3 dev board** ที่มี Flash 16 MB + PSRAM 8 MB
  - เช่นบอร์ดกลุ่ม Xiaozhi AI, หรือบอร์ด ESP32-S3 ที่สเปกใกล้เคียง
- สาย **USB Type-C** ที่เป็นสายข้อมูล (ไม่ใช่สายชาร์จอย่างเดียว)
- **Telegram Bot Token**
  - สร้างผ่านบัญชี [@BotFather](https://t.me/BotFather) ใน Telegram
- **API Key ของผู้ให้บริการ LLM อย่างน้อยหนึ่งตัว**
  - Anthropic API key — จาก [console.anthropic.com](https://console.anthropic.com)
  - หรือ OpenAI API key — จาก [platform.openai.com](https://platform.openai.com)
  - หรือ OpenRouter API key — จาก [openrouter.ai](https://openrouter.ai)
  - หรือ Gemini API key — จาก Google AI Studio (ใช้ endpoint แบบ OpenAI-compatible)

---

## การติดตั้ง (ESP-IDF)

ต้องติดตั้ง ESP-IDF v5.5+ ก่อนใช้งาน

- คู่มืออย่างเป็นทางการ:
  - https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/

เมื่อเตรียม ESP-IDF พร้อมแล้ว:

```bash
git clone https://github.com/memovai/mimiclaw.git
cd mimiclaw

idf.py set-target esp32s3
```

### ตัวอย่างติดตั้งบน Ubuntu

```bash
sudo apt-get update
sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

./scripts/setup_idf_ubuntu.sh
./scripts/build_ubuntu.sh
```

### ตัวอย่างติดตั้งบน macOS

```bash
xcode-select --install
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

./scripts/setup_idf_macos.sh
./scripts/build_macos.sh
```

---

## การตั้งค่าครั้งแรก (mimi_secrets.h)

ระบบใช้การตั้งค่า 2 ชั้น:

- ชั้นที่ 1: ค่าเริ่มต้นตอนคอมไพล์ (อยู่ในไฟล์ `main/mimi_secrets.h`)
- ชั้นที่ 2: ค่า runtime ที่ตั้งผ่าน Serial CLI (เก็บใน NVS บน Flash)

ค่าใน NVS จะมี priority สูงกว่าค่าใน `mimi_secrets.h`

เริ่มต้นด้วยการคัดลอกไฟล์ตัวอย่าง:

```bash
cp main/mimi_secrets.h.example main/mimi_secrets.h
```

เปิดไฟล์ `main/mimi_secrets.h` แล้วแก้ค่าพื้นฐาน เช่น:

```c
#define MIMI_SECRET_WIFI_SSID       "ชื่อWiFiของคุณ"
#define MIMI_SECRET_WIFI_PASS       "รหัสผ่านWiFiของคุณ"
#define MIMI_SECRET_TG_TOKEN        "123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11"
#define MIMI_SECRET_API_KEY         "sk-xxxxxx"          // API key เริ่มต้น
#define MIMI_SECRET_MODEL_PROVIDER  "anthropic"          // "anthropic", "openai", "openrouter" หรือ "gemini"
#define MIMI_SECRET_SEARCH_KEY      ""                   // ถ้าใช้ Brave Search ให้ใส่ key ที่นี่
#define MIMI_SECRET_PROXY_HOST      ""                   // ถ้าใช้ proxy ให้ใส่เช่น "192.168.1.10"
#define MIMI_SECRET_PROXY_PORT      ""                   // พอร์ตของ proxy เช่น "7897"
```

ค่าพวกนี้ไว้ใช้เป็นค่า default ถ้าคุณยังไม่เคยตั้งผ่าน CLI

---

## การคอมไพล์และแฟลชเฟิร์มแวร์

ทุกครั้งที่แก้ `mimi_secrets.h` แนะนำให้ fullclean ก่อน:

```bash
idf.py fullclean && idf.py build
```

หา serial port ของบอร์ด:

```bash
ls /dev/cu.usb*          # macOS
ls /dev/ttyACM*          # Linux
```

แฟลชและเปิด monitor:

```bash
idf.py -p PORT flash monitor
```

> สำคัญ: ส่วนใหญ่บอร์ด ESP32-S3 จะมีพอร์ต USB-C สองพอร์ต
> - ให้เสียบพอร์ตที่เขียนว่า **USB** (native USB Serial/JTAG)
> - อย่าเสียบพอร์ตที่เป็น USB-to-UART (บางทีเขียนว่า COM) ถ้าเสียบผิดจะแฟลชไม่ได้

---

## การตั้งค่าผ่าน Serial CLI

เมื่อบอร์ดบูตขึ้นมาแล้ว คุณสามารถต่อ serial monitor และพิมพ์คำสั่ง `mimi>` เพื่อสั่งงานและตั้งค่าระบบได้

ตัวอย่างการตั้งค่าหลักๆ:

```text
mimi> wifi_set MySSID MyPassword        # ตั้งค่า WiFi
mimi> set_tg_token 123456:ABC...        # ตั้งค่า Telegram Bot Token
mimi> set_api_key sk-xxxx...            # ตั้งค่า API key ของ LLM

# เลือกผู้ให้บริการ LLM
mimi> set_model_provider anthropic      # ใช้ Anthropic (Claude)
mimi> set_model_provider openai         # ใช้ OpenAI
mimi> set_model_provider openrouter     # ใช้ OpenRouter
mimi> set_model_provider gemini         # ใช้ Gemini (OpenAI-compatible endpoint)

# ตั้งชื่อโมเดล (ขึ้นกับ provider ที่เลือก)
mimi> set_model claude-opus-4-5         # ตัวอย่าง Anthropic
mimi> set_model gpt-4o                  # ตัวอย่าง OpenAI
mimi> set_model openrouter/your-model   # ตัวอย่าง OpenRouter (แล้วแต่โมเดล)
mimi> set_model gemini-1.5-flash        # ตัวอย่าง Gemini

# Proxy (เช่นต้องออกเน็ตผ่าน Clash/V2Ray)
mimi> set_proxy 192.168.1.83 7897       # ตั้งค่า HTTP CONNECT proxy
mimi> clear_proxy                        # ลบการตั้งค่า proxy

# Brave Search (ให้ LLM ค้นเว็บได้)
mimi> set_search_key BSA...             # Brave Search API key

# การจัดการ config อื่นๆ
mimi> config_show                       # แสดงค่าที่ตั้งทั้งหมด (ปิดบังส่วนสำคัญ)
mimi> config_reset                      # ล้าง NVS กลับไปใช้ค่าใน mimi_secrets.h
```

คำสั่งอื่นสำหรับ debug / maintenance:

```text
mimi> wifi_status              # ดูสถานะ WiFi
mimi> memory_read              # ดูไฟล์ MEMORY.md
mimi> memory_write "ข้อความ"   # เขียนลง MEMORY.md
mimi> heap_info                # ดู RAM คงเหลือ
mimi> session_list             # ดูรายชื่อ session แชตทั้งหมด
mimi> session_clear 12345      # ลบ session ตาม ID
mimi> heartbeat_trigger        # สั่ง heartbeat ให้ตรวจงานตอนนี้เลย
mimi> cron_start               # เริ่ม cron scheduler ทันที
mimi> restart                  # รีบูตบอร์ด
```

---

## การใช้ LLM หลายผู้ให้บริการ

แนวคิดหลักคือ MimiClaw มี proxy ชั้นกลาง (`llm_proxy`) ที่แปลง request/response ให้เข้ากับแต่ละผู้ให้บริการ โดย agent ด้านบนไม่ต้องเปลี่ยนโค้ด

- ถ้าใช้ **Anthropic**
  - ระบบจะใช้ endpoint `https://api.anthropic.com/v1/messages`
  - ฟิลด์ `system` อยู่ด้านบนของ JSON
  - ใช้ `max_tokens`
- ถ้าใช้ **OpenAI / OpenRouter / Gemini**
  - ใช้รูปแบบ OpenAI Chat Completions
  - endpoint:
    - OpenAI: `https://api.openai.com/v1/chat/completions`
    - OpenRouter: `https://openrouter.ai/api/v1/chat/completions`
    - Gemini: `https://generativelanguage.googleapis.com/v1beta/openai/chat/completions`
  - ใช้ `max_completion_tokens`
  - อ่านคำตอบจาก `choices[0].message.content`

คุณเพียง:

1. ตั้ง `set_model_provider` ให้ตรงกับผู้ให้บริการ
2. ตั้ง `set_api_key` เป็น key ของผู้ให้บริการนั้น
3. ตั้ง `set_model` ให้เป็นชื่อโมเดลที่ provider รองรับ

ส่วนที่เหลือ agent loop จะจัดการให้ทั้งหมด

---

## ระบบความจำ (Memory)

MimiClaw ใช้ไฟล์ข้อความบน Flash เก็บ context ต่างๆ ที่เกี่ยวกับบอท:

| ไฟล์ | หน้าที่ |
|------|--------|
| `SOUL.md` | บุคลิกของบอท (system prompt หลัก) |
| `USER.md` | ข้อมูลเกี่ยวกับตัวคุณ เช่น ชื่อ ภาษาที่ใช้ สิ่งที่ชอบ |
| `MEMORY.md` | ความจำระยะยาว สิ่งที่อยากให้บอทจำต่อเนื่อง |
| `HEARTBEAT.md` | รายการงานที่ให้บอทคอยเช็กเป็นระยะ |
| `cron.json` | ตาราง cron งานที่บอทตั้งเอง เช่นงานประจำวัน |
| `yyyy-mm-dd.md` | ไฟล์บันทึกประจำวัน |
| `tg_xxx.jsonl` | history การคุยกับคุณผ่าน Telegram |

คุณสามารถเมานต์ SPIFFS แล้วแก้ไฟล์เหล่านี้ได้โดยตรง ถ้าต้องการ fine-tune พฤติกรรมของบอท

---

## Tools ที่ LLM เรียกใช้ได้

เพื่อให้บอททำงานได้มากกว่าการตอบข้อความอย่างเดียว MimiClaw รองรับการเรียกใช้ tools จากใน LLM:

| Tool | อธิบาย |
|------|--------|
| `web_search` | ค้นเว็บผ่าน Brave Search API |
| `get_current_time` | ขอเวลา/วันที่ปัจจุบันผ่าน HTTP และ sync เวลาให้บอร์ด |
| `cron_add` | สร้าง cron job (งานแบบครั้งเดียวหรือซ้ำ) ให้รันอัตโนมัติ |
| `cron_list` | ดูรายการ cron job ที่ตั้งไว้ทั้งหมด |
| `cron_remove` | ลบ cron job ด้วย ID |

เพื่อเปิดใช้ web search:

- ใส่ Brave Search API key ใน `MIMI_SECRET_SEARCH_KEY` ในไฟล์ `mimi_secrets.h`
- หรือใช้คำสั่ง `set_search_key` ผ่าน CLI

เวลา LLM ต้องการค้นเว็บ มันจะเรียก tool `web_search` แล้วส่งผลลัพธ์กลับไปคิดต่อ

---

## Cron Tasks และ Heartbeat

### Cron Tasks

MimiClaw มี cron scheduler ในตัว ทำให้ LLM สามารถ:

- ตั้งงานให้รันทุกๆ N วินาที/นาที/ชั่วโมง
- ตั้งงานให้รันครั้งเดียวในเวลาที่กำหนด (unix timestamp)

ทุกงานจะถูกเก็บในไฟล์ `cron.json` บน SPIFFS และคงอยู่แม้บอร์ดรีบูต

ตัวอย่าง use case:

- สรุปสิ่งที่ทำในแต่ละวัน
- เตือนให้ทำนัด/ทานยา/ออกกำลังกาย
- ตรวจเช็กบางข้อมูลจากอินเทอร์เน็ตเป็นระยะ

### Heartbeat

บริการ heartbeat จะตื่นขึ้นมาเป็นช่วงๆ (ตั้งค่าใน config) เพื่อ:

1. อ่านไฟล์ `HEARTBEAT.md`
2. ดูว่ามีรายการงานที่ยังไม่เสร็จอยู่หรือไม่
3. ถ้ามี จะสร้างข้อความใหม่เข้า agent loop เพื่อให้ LLM ตัดสินใจทำงานนั้น

ดังนั้นคุณสามารถเขียน task ง่ายๆ ไว้ใน `HEARTBEAT.md` แล้วปล่อยให้บอทจัดการเอง เช่น:

- "ทุกเช้า สรุปสิ่งที่ต้องทำวันนี้ให้ฉัน"
- "ทุกเย็น สรุปสิ่งที่เกิดขึ้นวันนี้"

---

## ฟีเจอร์อื่นๆ ที่มีในตัว

- WebSocket gateway ที่พอร์ต 18789
- OTA update (แฟลชเฟิร์มแวร์ใหม่ผ่าน WiFi)
- ระบบทำงานแบบ multi-core แยกงาน I/O กับงาน AI
- รองรับ HTTP CONNECT proxy เหมาะกับเครือข่ายที่ต้องออกเน็ตผ่าน proxy
- Agent loop ที่รองรับ tool calling ทั้งฝั่ง Anthropic และ OpenAI-compatible

---

## สำหรับนักพัฒนา

รายละเอียดเชิงเทคนิคอยู่ในโฟลเดอร์ `docs/`:

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — อธิบายโครงสร้างระบบ โมดูล หน่วยความจำ โปรโตคอล และพาร์ทิชันของ Flash
- [docs/TODO.md](docs/TODO.md) — รายการฟีเจอร์ที่ยังไม่เสร็จและ roadmap

หากคุณต้องการดู implementation หลักของ LLM proxy สามารถดูได้ที่:

- main/llm/llm_proxy.c
- main/llm/llm_proxy.h

---

## การมีส่วนร่วม (Contributing)

ถ้าคุณเจอบั๊กหรืออยากเพิ่มฟีเจอร์:

- อ่านไฟล์ [docs/CONTRIBUTE.md](docs/CONTRIBUTE.md) ก่อน
- จากนั้นจึงเปิด issue หรือ pull request ใน GitHub

---

## ใบอนุญาต

โปรเจกต์นี้ใช้สัญญาอนุญาตแบบ MIT ดูรายละเอียดในไฟล์ LICENSE

