# Browser Extension Bridge (Preview)

This directory contains an experimental browser automation bridge for MimiClaw.

It combines:

- a Chrome MV3 extension (`manifest.json`, `background.js`, `content.js`, `offscreen.js`, `popup.*`), and
- a local Python simulator (`esp32_sim_server.py`) that mimics the ESP32 side over WebSocket.

## What This Is For

Use this when you want to validate browser control workflows before wiring the same protocol into firmware.

| Path | Best for | Runs where |
|------|----------|------------|
| MimiClaw firmware (`main/`) | On-device assistant + Telegram + tools | ESP32-S3 |
| Extension bridge (`extension/`) | Browser interaction experiments and action-loop tuning | Desktop browser + local Python |

The two paths share the same message pattern (`get_dom_snapshot` / `execute_action` / `command_result`) so logic can be migrated incrementally.

## Quick Install

### 1. Start the local simulator

```bash
cd extension
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env
# edit .env and set OPENAI_API_KEY
export $(grep -v '^#' .env | xargs)
python esp32_sim_server.py
```

Default endpoint: `ws://127.0.0.1:8765/ws`

### 2. Load the extension in Chrome

1. Open `chrome://extensions`
2. Enable `Developer mode`
3. Click `Load unpacked`
4. Select this `extension/` directory

### 3. Run a task

1. Open a target webpage.
2. In the extension popup, verify connection status and keep listener enabled.
3. In the Python terminal, enter a goal, for example:

```text
user> Click the "Sign in" button on this page.
```

4. The simulator requests DOM state, asks the LLM for one action, sends it to the extension, and repeats until completion.

## Supported Actions

- `navigate` (`url`)
- `back`
- `forward`
- `click` (`selector` or `text`)
- `fill` (`selector`, `value`)
- `scroll` (`top`)

## Message Protocol (Summary)

Python simulator -> extension:

- `{"type":"get_dom_snapshot","request_id":"..."}`
- `{"type":"execute_action","request_id":"...","action":{...}}`

Extension -> Python simulator:

- `{"type":"command_result","request_id":"...","ok":true,"result":{...}}`

## Notes

- This is a prototype bridge. It does not change firmware behavior by itself.
- Some websites enforce CSP/cross-origin limits that can reduce extraction/action reliability.
- Offscreen + heartbeat/re-register logic is used to keep the WebSocket bridge stable under MV3 service-worker lifecycle constraints.

## Troubleshooting

- Popup shows disconnected: verify `esp32_sim_server.py` is running and `ws://127.0.0.1:8765/ws` is reachable.
- Actions do not execute: keep the target tab on an `http://` or `https://` page (not `chrome://` or extension pages).
- No LLM output: confirm `OPENAI_API_KEY` is exported in the simulator shell.
