# Board B - MicroPython Executor

Board B is an ESP32 (or ESP32-S3) running MicroPython that receives Python scripts from Board A (MimiClaw) via ESP-NOW and executes them.

## Requirements

- ESP32 or ESP32-S3 board
- MicroPython firmware with ESP-NOW support (v1.20+)

## Setup

### 1. Flash MicroPython firmware

Download the latest MicroPython firmware for your board from https://micropython.org/download/

```bash
# Erase flash
esptool.py --chip esp32s3 erase_flash

# Flash MicroPython (adjust firmware filename)
esptool.py --chip esp32s3 write_flash -z 0 ESP32_GENERIC_S3-20240602-v1.23.0.bin
```

### 2. Upload scripts

```bash
# Install mpremote
pip install mpremote

# Upload boot.py and main.py
mpremote cp boot.py :boot.py
mpremote cp main.py :main.py

# Reset the board
mpremote reset
```

### 3. Get Board B MAC address

After uploading and resetting, Board B will print its MAC address:

```
Board B MAC: aa:bb:cc:dd:ee:ff
On Board A, run: set_espnow_peer aa:bb:cc:dd:ee:ff
```

### 4. Configure Board A

On Board A's serial console:

```
set_espnow_peer aa:bb:cc:dd:ee:ff
restart
```

## How It Works

1. Board A's AI agent decides to run a Python script (via `run_python` tool)
2. The script is sent to Board B over ESP-NOW (chunked, 243 bytes per packet)
3. Board B receives and reassembles the script
4. Board B executes the script with `exec()`, capturing stdout
5. The result (stdout + any exceptions) is sent back to Board A
6. Board A returns the result to the AI agent

## Limitations

- ESP-NOW range: ~200m line of sight, ~50m indoors
- MicroPython memory: limited by Board B's available RAM
- No persistent state between script executions (each `exec()` gets a fresh namespace)
- No WiFi connection on Board B (WiFi STA is active but not connected, used only for ESP-NOW)
