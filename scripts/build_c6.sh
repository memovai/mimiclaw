#!/usr/bin/env bash
# Build/flash helper for ESP32-C6 port.
# Usage:
#   ./scripts/build_c6.sh            # build only
#   ./scripts/build_c6.sh flash      # build + full flash (resets SPIFFS)
#   ./scripts/build_c6.sh app-flash  # build + app-only flash (preserves SPIFFS)
#   ./scripts/build_c6.sh monitor    # open serial monitor
set -euo pipefail

IDF_PATH="${IDF_PATH:-/Users/jamesfield/esp/esp-idf}"
IDF_PYTHON_ENV="${IDF_PYTHON_ENV:-/Users/jamesfield/.espressif/python_env/idf6.0_py3.12_env}"
PORT="${PORT:-/dev/cu.usbmodem142101}"
BAUD="${BAUD:-460800}"

export IDF_PATH
export PATH="$IDF_PYTHON_ENV/bin:$IDF_PATH/tools:$PATH"

PYTHON="$IDF_PYTHON_ENV/bin/python"
IDF="$PYTHON $IDF_PATH/tools/idf.py"

cd "$(dirname "${BASH_SOURCE[0]}")/.."

ACTION="${1:-build}"

case "$ACTION" in
  build)
    $IDF build
    ;;
  flash)
    $IDF build && $IDF -p "$PORT" -b "$BAUD" flash
    ;;
  app-flash)
    $IDF build && $IDF -p "$PORT" -b "$BAUD" app-flash
    ;;
  monitor)
    $IDF -p "$PORT" monitor
    ;;
  *)
    echo "Unknown action: $ACTION" >&2
    echo "Usage: $0 [build|flash|app-flash|monitor]" >&2
    exit 1
    ;;
esac
