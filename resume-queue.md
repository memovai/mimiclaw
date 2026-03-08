# Resume Queue

## Primary Resume Target

Use the now-validated `ATK-DNESP32S3` + `Feishu` + OpenAI-compatible LLM
baseline to decide the next engineering step after first real channel success:
either clean up doc drift, promote runtime config to build-time defaults, or
continue with additional channel / tool validation.

## Secondary Pending Items

- Decide whether to update `docs/ARCHITECTURE.md` and Feishu docs to match the
  current runtime-config + WebSocket-only implementation
- Only create `main/mimi_secrets.h` later if persistent build-time defaults are
  preferred over the already working NVS configuration
- Decide whether to validate Telegram next or stay focused on Feishu-first
  hardening

## Latest Verified State

- The repo is an `ESP-IDF` firmware project for `ESP32-S3`
- It expects `16MB` flash, `8MB` PSRAM, Wi-Fi, and a serial / USB console
- It does not currently appear to depend on LCD, camera, TF, or audio hardware
- The local `ATK-DNESP32S3` board matches the required MCU / flash / PSRAM on paper
- Prior hardware bring-up outside this repo already validated:
  - red status LED control on `GPIO1` (active-low)
  - LCD output on the `ST7789V` path after the `XL9555` board-control fix
  - TF / microSD mount + read/write on the shared LCD/TF SPI bus
- The repo constraint from `main/idf_component.yml`
  (`ESP-IDF >=5.5.0,<5.6.0`) is now satisfied by the local working environment
- Version choice has been decided: use `ESP-IDF v5.5.2` as the next build target
- The working local environment for this repo is:
  - `ESP-IDF` checkout: `/Users/lize/.espressif/esp-idf-v5.5.2`
  - Python env: `/Users/lize/.espressif/python_env/idf5.5_py3.14_env`
- `IDF_DIR=/Users/lize/.espressif/esp-idf-v5.5.2 /bin/bash scripts/build_macos.sh`
  now completes successfully on this machine
- The compiled image was flashed successfully to `/dev/cu.usbserial-10` at
  `460800`
- This board's known-good host interaction path remains:
  - flash on `/dev/cu.usbserial-10` at `460800`
  - serial / CLI at `115200`
- Serial verification at `115200` confirmed:
  - bootloader and app both report `ESP-IDF v5.5.2`
  - PSRAM detection succeeds (`8MB`)
  - SPIFFS mounts successfully
  - the serial CLI prompt is reachable as `mimi>`
  - `help` returns the expected command list
- Board-level implication for `mimiclaw`:
  prior LED / LCD / TF bring-up success means current work should stay focused
  on runtime configuration and network/channel validation unless fresh hardware
  symptoms appear
- `main/mimi_secrets.h` is currently absent, so the flashed firmware boots into
  an NVS-configured state rather than relying on build-time secrets
- Runtime credentials are now configured in NVS on the board:
  - Wi-Fi SSID `mynet`
  - Feishu App ID / App Secret
  - provider `openai`
  - model `qwen3.5-plus`
  - base URL `https://coding.dashscope.aliyuncs.com/v1`
  - API key set
- The firmware now includes a new CLI command `set_base_url <url>` and a new
  NVS-backed `Base URL` field in `config_show`
- The updated firmware was rebuilt under the validated local `ESP-IDF v5.5.2`
  environment and reflashed successfully to `/dev/cu.usbserial-10`
- Serial verification after the reflash confirmed:
  - `llm_proxy` initializes as `provider: openai, model: qwen3.5-plus`
  - Feishu credentials load from NVS
  - Feishu WebSocket mode starts and reaches `Feishu WS connected`
  - Wi-Fi still reconnects successfully and gets `192.168.31.165`
- Host-driven WebSocket validation against the board gateway confirmed the
  OpenAI-compatible LLM path is live:
  - a local test message `Reply with exactly: pong` was delivered into the
    agent loop
  - the board called the configured LLM endpoint with provider `openai`
  - the final assistant response returned as `pong`
- The user has now confirmed the real Feishu in-chat path also works end-to-end
  on hardware, so the first true channel validation is complete
- This repo has adopted the markdown-first layered memory system in this file set

## Next Concrete Step

Keep the current flashed firmware and move from first-channel bring-up into
cleanup / next-target planning:

1. Update drifted docs so they match reality:
   - runtime config exists
   - CLI command is `set_wifi`, not `wifi_set`
   - Feishu channel runs in WebSocket mode, not the older webhook-only story
2. Decide whether the current working NVS config should remain runtime-only or
   be represented by a local `main/mimi_secrets.h` for reproducibility
3. Choose the next validation target:
   - Telegram channel
   - Feishu hardening / permissions / UX
   - tool coverage beyond the basic reply path

## Resume Reading Path

For the next session, read:

1. `AGENTS.md`
2. `resume-queue.md`
3. `MEMORY.md`
4. `README.md`
5. `docs/ARCHITECTURE.md`
6. `main/idf_component.yml`
7. the latest file in `memory/`
