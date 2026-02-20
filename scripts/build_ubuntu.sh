#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_VERSION="${IDF_VERSION:-v5.5.2}"
ESP_ROOT="${ESP_ROOT:-$HOME/.espressif}"
DEFAULT_IDF_DIR="$ESP_ROOT/esp-idf-$IDF_VERSION"
IDF_DIR="${IDF_DIR:-${IDF_PATH:-$DEFAULT_IDF_DIR}}"

if [[ ! -f "$IDF_DIR/export.sh" ]]; then
  echo "ESP-IDF not found at: $IDF_DIR" >&2
  echo "Run scripts/setup_idf_ubuntu.sh first, or set IDF_DIR/IDF_PATH." >&2
  exit 1
fi

# shellcheck source=/dev/null
. "$IDF_DIR/export.sh"

cd "$PROJECT_ROOT"
idf.py set-target esp32s3
idf.py build
