#!/usr/bin/env bash
# MimiClaw Quick Deploy Script
# Usage: ./skills/deploy/scripts/deploy.sh [port]
#
# This script handles the full build-flash cycle:
# 1. Checks prerequisites
# 2. Ensures mimi_secrets.h exists
# 3. Builds the firmware
# 4. Auto-detects or uses specified serial port
# 5. Flashes and opens monitor

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[x]${NC} $*"; exit 1; }

# Find project root (where this script lives: skills/deploy/scripts/)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$PROJECT_ROOT"

info "MimiClaw Deploy — project: $PROJECT_ROOT"

# Check ESP-IDF
if ! command -v idf.py &>/dev/null; then
    error "ESP-IDF not found. Source export.sh first:\n  source \$IDF_PATH/export.sh"
fi

IDF_VER=$(idf.py --version 2>&1 | head -1)
info "ESP-IDF: $IDF_VER"

# Check secrets
if [ ! -f main/mimi_secrets.h ]; then
    warn "main/mimi_secrets.h not found — creating from example"
    cp main/mimi_secrets.h.example main/mimi_secrets.h
    warn "Edit main/mimi_secrets.h with your credentials, then re-run this script"
    exit 1
fi

# Check if secrets are configured (WiFi SSID not empty)
if grep -q 'MIMI_SECRET_WIFI_SSID.*""' main/mimi_secrets.h; then
    error "WiFi SSID is empty in main/mimi_secrets.h — edit it first"
fi

# Build
info "Building firmware (fullclean)..."
idf.py fullclean >/dev/null 2>&1 || true
idf.py build 2>&1 | tail -5

if [ ! -f build/mimiclaw.bin ]; then
    error "Build failed — check errors above"
fi

BIN_SIZE=$(stat -f%z build/mimiclaw.bin 2>/dev/null || stat -c%s build/mimiclaw.bin 2>/dev/null)
info "Firmware built: build/mimiclaw.bin ($(( BIN_SIZE / 1024 )) KB)"

# Detect serial port
PORT="${1:-}"
if [ -z "$PORT" ]; then
    # Auto-detect
    if [ "$(uname)" = "Darwin" ]; then
        PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
    else
        PORT=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1)
    fi
fi

if [ -z "$PORT" ]; then
    error "No serial port found. Plug in your ESP32-S3 or specify port:\n  $0 /dev/cu.usbmodem1101"
fi

info "Serial port: $PORT"

# Flash
info "Flashing..."
idf.py -p "$PORT" flash 2>&1 | tail -10

info "Flash complete!"
echo ""
info "Opening serial monitor (Ctrl+] to exit)..."
echo ""

idf.py -p "$PORT" monitor
