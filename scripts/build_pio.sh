#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

echo "=== PlatformIO Build ==="
echo "Building firmware..."
pio run

echo ""
echo "=== Uploading firmware ==="
pio run -t upload

echo ""
echo "=== Uploading SPIFFS filesystem ==="
pio run -t uploadfs

echo ""
echo "=== Starting serial monitor (Ctrl+] to exit) ==="
pio device monitor