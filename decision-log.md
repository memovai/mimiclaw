# Decision Log

## 2026-03-08 - Adopt markdown-first layered memory system

Decision:

- Use a local markdown-first memory system for cross-session continuity in this
  repo.

Why:

- Chat context is not durable
- The repo did not yet have a structured resume / memory layer
- The work benefits from separating rules, pending queue, stable memory, daily
  memory, fact docs, and decisions

Consequences:

- `AGENTS.md` is the control plane for read order and session boundaries
- `resume-queue.md` is the default resume entrypoint
- `MEMORY.md` stores only stable repo-level reminders
- `memory/YYYY-MM-DD.md` stores daily working memory

Rollback:

- If this structure proves too heavy for the repo, keep `AGENTS.md`,
  `resume-queue.md`, and `MEMORY.md` as the minimum durable core

## 2026-03-08 - Choose ESP-IDF v5.5.2 for the next validation cycle

Decision:

- Use `ESP-IDF v5.5.2` as the next version for build and hardware validation in
  this repo.

Five axioms used:

1. explicit upstream compatibility constraints outrank local convenience
2. within an allowed minor line, prefer the latest stable bugfix release
3. do not widen the version interval before the baseline build is proven
4. do not use a pre-release when a matching stable release exists
5. choose the path with the cheapest rollback and the clearest success criteria

Why:

- `main/idf_component.yml` explicitly requires `>=5.5.0,<5.6.0`
- `v5.5.2` is the latest stable bugfix release in the allowed `5.5.x` line
- local visible checkouts are both `v5.1.2`, which are outside the repo's
  allowed range
- `v6.0-beta1` exists but is pre-release and adds unnecessary migration risk
  before first baseline validation

Consequences:

- next environment work should target `v5.5.2`, not `v5.4.x`, `v5.1.x`, or
  `v6.0-beta1`
- any build failure after moving to `v5.5.2` should be treated as a project or
  integration issue, not as an expected version mismatch

Rollback:

- if `v5.5.2` exposes a concrete blocker specific to this repo or board, stay
  within the allowed `5.5.x` line first; only reconsider the minor version
  boundary after evidence exists

## 2026-03-08 - Reuse the existing ESP-IDF v5.5.2 install and configure on-device first

Decision:

- Reuse the existing local `ESP-IDF v5.5.2` install at
  `/Users/lize/.espressif/esp-idf-v5.5.2`, keep the already flashed image on
  the board, and do the first integration validation through the serial CLI
  rather than creating build-time secrets first.

Why:

- the existing `v5.5.2` checkout and Python environment were already complete
  and worked immediately for build + flash
- the board already booted successfully into `MimiClaw` and exposed the `mimi>`
  CLI prompt
- runtime configuration through CLI is the fastest path to validate Wi-Fi and
  the first channel without adding another rebuild cycle

Consequences:

- next work should start from the flashed firmware already on
  `/dev/cu.usbserial-10`
- configure Wi-Fi, channel credentials, and API key over serial first
- only create `main/mimi_secrets.h` later if persistent build-time defaults are
  actually desired

Rollback:

- if CLI-driven runtime configuration proves insufficient or inconvenient,
  create `main/mimi_secrets.h`, rebuild, and reflash using the same validated
  `ESP-IDF v5.5.2` environment

## 2026-03-08 - Persist the collaboration protocol itself in repository memory

Decision:

- Treat the agent-layer collaboration protocol as durable repository context and
  persist it locally under `AGENTS.md` / `MEMORY.md`, not only as chat
  convention.

Why:

- the user explicitly uses `开口` / `收口` as session control commands
- the user explicitly wants agent interaction rules and memory behavior to live
  under `mimiclaw`
- this reduces re-discovery cost and keeps cross-session collaboration
  consistent

Consequences:

- reopen / close behavior should be recoverable from local files alone
- stable interaction expectations belong in the same memory system as technical
  context
- when future interaction conventions become durable, store them locally rather
  than leaving them implicit

Rollback:

- if this proves too noisy, keep only the minimal explicit protocol in
  `AGENTS.md` and trim non-essential collaboration notes from `MEMORY.md`

## 2026-03-08 - Optimize for seamless switching from esp32 into mimiclaw

Decision:

- Treat switching from `/Users/lize/workspace/esp32` into this repo as a
  seamless continuation path, not as a fresh cold start.

Why:

- the current `mimiclaw` work directly depends on verified board and workflow
  context established in the sibling `esp32` workspace
- the user explicitly wants cross-workspace continuation without repeated
  re-briefing

Consequences:

- imported board knowledge and collaboration protocol must be sufficient for
  fast recovery inside this repo
- when resuming here after work in `esp32`, read local memory first and proceed
  from the stored handoff instead of asking the user to restate background

Rollback:

- if the imported context becomes stale or misleading, keep the seamless-switch
  goal but update the local memory with corrected facts rather than abandoning
  the approach

## 2026-03-08 - Add runtime-configurable LLM base URL support for OpenAI-compatible providers

Decision:

- Add an NVS-backed/runtime-configurable `LLM base URL` to the firmware and
  expose it through the serial CLI as `set_base_url`.

Why:

- the existing implementation hard-coded `api.openai.com`, which blocked use of
  OpenAI-compatible providers such as DashScope
- the current validation target needed `https://coding.dashscope.aliyuncs.com/v1`
  with provider `openai` and model `qwen3.5-plus`
- runtime configuration keeps the operator path consistent with the existing
  Wi-Fi / channel / model / API-key workflow

Consequences:

- `llm_proxy` now persists and loads `base_url` from NVS
- `config_show` reports the configured `Base URL`
- OpenAI-compatible base URLs ending in `/v1` are normalized to
  `/v1/chat/completions`
- the proxy code no longer assumes `api.openai.com`; it parses host / path from
  the configured URL
- the board can now validate OpenAI-compatible providers without creating
  `main/mimi_secrets.h`

Rollback:

- if the custom URL layer proves unstable, clear the NVS key and fall back to
  the provider defaults; if necessary, remove the `set_base_url` path and
  return to fixed upstream endpoints

## 2026-03-08 - Use Feishu as the first real validated channel

Decision:

- Treat `Feishu` as the first successfully validated real channel path for the
  local board, ahead of Telegram.

Why:

- the user provided Feishu credentials first
- the board successfully established Feishu WebSocket connectivity
- the user confirmed that real Feishu in-chat messaging works end-to-end after
  the OpenAI-compatible LLM path was configured

Consequences:

- subsequent work can start from a known-good Feishu baseline instead of
  treating channel validation as still blocked
- the next engineering focus should shift from first-message bring-up to
  documentation cleanup, reproducibility, or additional channel validation

Rollback:

- if later evidence shows the Feishu path is unstable or environment-specific,
  keep the implementation but downgrade this from a baseline assumption until
  reliability is re-established
