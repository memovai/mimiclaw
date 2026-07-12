#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_VERSION="${IDF_VERSION:-v5.5.2}"
ESP_ROOT="${ESP_ROOT:-$HOME/.espressif}"
DEFAULT_IDF_DIR="$ESP_ROOT/esp-idf-$IDF_VERSION"
IDF_DIR="${IDF_DIR:-${IDF_PATH:-$DEFAULT_IDF_DIR}}"
BUILD_DIR="${BUILD_DIR:-build_macos}"

pick_python() {
  local py
  for py in "${ESP_PYTHON:-}" /opt/homebrew/bin/python3 python3; do
    [[ -n "$py" ]] || continue
    command -v "$py" >/dev/null 2>&1 || continue
    if "$py" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)' >/dev/null 2>&1; then
      command -v "$py"
      return 0
    fi
  done
  echo "Python >= 3.10 not found. Run scripts/setup_idf_macos.sh first." >&2
  return 1
}

if [[ ! -f "$IDF_DIR/export.sh" ]]; then
  echo "ESP-IDF not found at: $IDF_DIR" >&2
  echo "Run scripts/setup_idf_macos.sh first, or set IDF_DIR/IDF_PATH." >&2
  exit 1
fi

export ESP_PYTHON="$(pick_python)"
unset IDF_PYTHON_ENV_PATH
echo "python: using $ESP_PYTHON"

# shellcheck source=/dev/null
. "$IDF_DIR/export.sh"

cd "$PROJECT_ROOT"
if ! grep -q '^CONFIG_IDF_TARGET="esp32s3"' sdkconfig 2>/dev/null; then
  idf.py -B "$BUILD_DIR" set-target esp32s3
fi
idf.py -B "$BUILD_DIR" build
