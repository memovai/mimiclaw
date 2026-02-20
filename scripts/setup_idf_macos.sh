#!/usr/bin/env bash
set -euo pipefail

if [[ "${OSTYPE:-}" != "darwin"* ]]; then
  echo "This setup script currently supports macOS only." >&2
  exit 1
fi

IDF_VERSION="${IDF_VERSION:-v5.5.2}"
ESP_ROOT="${ESP_ROOT:-$HOME/.espressif}"
IDF_DIR="${IDF_DIR:-$ESP_ROOT/esp-idf-$IDF_VERSION}"

if ! command -v brew >/dev/null 2>&1; then
  echo "Homebrew not found. Install it first:" >&2
  echo "  /bin/bash -c \"\$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)\"" >&2
  exit 1
fi

ensure_brew_pkg() {
  local pkg="$1"
  if brew list --formula --versions "$pkg" >/dev/null 2>&1; then
    echo "brew: $pkg already installed, skipping"
  else
    if ! brew install "$pkg"; then
      echo "warn: failed to install $pkg via brew; continuing" >&2
      return 0
    fi
  fi
}

ensure_brew_pkg_if_missing_cmd() {
  local pkg="$1"
  local cmd="$2"
  if command -v "$cmd" >/dev/null 2>&1; then
    echo "cmd: $cmd already available, skipping brew $pkg"
  else
    ensure_brew_pkg "$pkg"
  fi
}

ensure_brew_pkg_if_missing_cmd git git
ensure_brew_pkg_if_missing_cmd wget wget
ensure_brew_pkg_if_missing_cmd flex flex
ensure_brew_pkg_if_missing_cmd bison bison
ensure_brew_pkg_if_missing_cmd gperf gperf
ensure_brew_pkg_if_missing_cmd python python3
ensure_brew_pkg_if_missing_cmd cmake cmake
ensure_brew_pkg_if_missing_cmd ninja ninja
ensure_brew_pkg_if_missing_cmd ccache ccache
ensure_brew_pkg_if_missing_cmd dfu-util dfu-util
ensure_brew_pkg libusb
ensure_brew_pkg libffi
ensure_brew_pkg openssl@3

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
