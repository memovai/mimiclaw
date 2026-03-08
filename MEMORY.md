# MEMORY.md

## Stable Repo-Level Memory

- This repo should use markdown files as the durable collaboration control
  plane; do not rely on chat context alone.
- Default recovery entrypoint is `resume-queue.md`, not `MEMORY.md`.
- Stable user / agent collaboration reminder:
  the user expects durable context, protocol, and important interaction
  agreements to be written into this repo's local memory files instead of being
  left only in chat history.
- Stable session-control reminder:
  `开口` means resume from the repo's local handoff state, and `收口` means
  produce a real local checkpoint / handoff update, not just a conversational
  summary.
- Stable cross-workspace collaboration reminder:
  when the user moves from `/Users/lize/workspace/esp32` into
  `/Users/lize/workspace/mimiclaw` to continue related work, the transition is
  expected to be seamless; this repo should already contain enough imported
  memory to resume without forcing the user to restate prior board context.
- For repo facts, prefer:
  - `README.md` for workflow and operator-facing setup
  - `docs/ARCHITECTURE.md` for module map and runtime architecture
  - `main/idf_component.yml` for required `ESP-IDF` range
  - `sdkconfig.defaults.esp32s3` and `partitions.csv` for platform baseline
  - implementation files for actual behavior
- For collaboration facts, prefer:
  - `AGENTS.md` for protocol and memory-layer rules
  - `resume-queue.md` for the next concrete resume point
  - `MEMORY.md` for stable collaboration reminders and cross-session
    constraints
  - `memory/YYYY-MM-DD.md` for fresh interaction context that is not yet stable
- When evaluating hardware fit, separate:
  - board hardware compatibility
  - host build environment compatibility
- Current important compatibility reminder:
  this repo requires `ESP-IDF >=5.5.0,<5.6.0`; do not assume an older working
  `ESP-IDF` from another repository is acceptable here.
- Verified host environment for this repo on this machine:
  `ESP-IDF v5.5.2` at `/Users/lize/.espressif/esp-idf-v5.5.2` with Python env
  `/Users/lize/.espressif/python_env/idf5.5_py3.14_env`.
- Verified board baseline for the local `ATK-DNESP32S3`:
  `mimiclaw` builds, flashes to `/dev/cu.usbserial-10` at `460800`, boots, and
  exposes a working serial CLI prompt at `115200`.
- Stable hardware profile for the local `ATK-DNESP32S3` board:
  `ESP32-S3R8`, `16MB` flash, `8MB` PSRAM, `ST7789V 320x240` SPI LCD,
  shared-SPI `TF / microSD`, `OV2640` camera path on the dedicated camera
  peripheral, and onboard audio hardware.
- Previously verified board bring-up facts from the dedicated hardware repo:
  - the controllable onboard LED is the red status LED on `GPIO1`, active-low
  - LCD bring-up required the `XL9555` I/O expander at `0x20` to explicitly
    drive LCD power / backlight and reset; assuming LCD control pins were `NC`
    caused a black screen even when firmware logs looked healthy
  - the LCD and TF card share the same SPI bus; TF smoke passed only after a
    card was inserted before boot or reset
  - `460800` is the known-good flash baud on this board; `921600` previously
    proved unstable
- For `mimiclaw` specifically, treat LED / LCD / TF board bring-up as already
  de-risked background knowledge, not as the default suspected cause of core
  boot or CLI failures unless new evidence points there.
- Stable interaction preference observed in this repo:
  when hardware knowledge, workflow protocol, or collaboration expectations
  become relevant to `mimiclaw`, persist them locally in this repo instead of
  expecting the next session to recover them from another repository or from
  chat alone.
- Stable runtime capability reminder:
  this repo now supports an NVS-backed/customizable `LLM base URL` via the CLI
  command `set_base_url`; this is required for OpenAI-compatible providers that
  are not `api.openai.com`.
- Stable local validation reminder:
  on the local `ATK-DNESP32S3`, the first fully verified real channel path is
  `Feishu` using an OpenAI-compatible backend with custom `base_url` support.
