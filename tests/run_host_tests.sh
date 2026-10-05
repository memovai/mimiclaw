#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
test_binary="$(mktemp "${TMPDIR:-/tmp}/mimiclaw-mqtt-tests.XXXXXX")"
trap 'rm -f "$test_binary"' EXIT

cc \
  -std=c11 \
  -Wall \
  -Wextra \
  -Werror \
  -pedantic \
  -I"$repo_root/main" \
  "$repo_root/tests/test_mqtt_message.c" \
  "$repo_root/main/channels/mqtt/mqtt_message.c" \
  -o "$test_binary"

"$test_binary"
