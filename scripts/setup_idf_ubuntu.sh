#!/usr/bin/env bash
set -euo pipefail

if [[ "${OSTYPE:-}" != "linux-gnu"* ]]; then
  echo "This setup script currently supports Ubuntu/Debian only." >&2
  exit 1
fi

IDF_VERSION="${IDF_VERSION:-v5.5.2}"
ESP_ROOT="${ESP_ROOT:-$HOME/.espressif}"
IDF_DIR="${IDF_DIR:-$ESP_ROOT/esp-idf-$IDF_VERSION}"

if [[ -f /etc/os-release ]]; then
  . /etc/os-release
  if [[ "${ID:-}" != "ubuntu" && "${ID_LIKE:-}" != *"debian"* ]]; then
    echo "Detected ${PRETTY_NAME:-unknown}. Continuing, but package installation assumes apt." >&2
  fi
fi

sudo apt-get update
sudo apt-get install -y \
  git wget flex bison gperf python3 python3-pip python3-venv \
  cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0

mkdir -p "$ESP_ROOT"
if [[ ! -d "$IDF_DIR/.git" ]]; then
  git clone --depth 1 --branch "$IDF_VERSION" --recursive \
    https://github.com/espressif/esp-idf.git "$IDF_DIR"
else
  git -C "$IDF_DIR" fetch --tags --depth 1 origin "$IDF_VERSION"
  git -C "$IDF_DIR" checkout "$IDF_VERSION"
  git -C "$IDF_DIR" submodule update --init --recursive
fi

"$IDF_DIR/install.sh" esp32s3

echo
echo "ESP-IDF installed. For current shell run:"
echo "  . \"$IDF_DIR/export.sh\""
echo "Then run from project root:"
echo "  idf.py set-target esp32s3"
echo "  idf.py build"
