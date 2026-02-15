# mimibrowser

This directory contains a working prototype:

- `esp32_sim_server.py`: a Python process that simulates the ESP32 controller, accepts user goals, and asks the LLM for next actions.
- Chrome extension (`mimibrowser`): captures DOM snapshots from the active page and executes LLM-driven browser actions (back/forward/navigate/click/fill/scroll).

## 1. Start the Python server

```bash
cd extension
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
cp .env.example .env
# 编辑 .env 填写 OPENAI_API_KEY
export $(grep -v '^#' .env | xargs)
python esp32_sim_server.py
```

Server endpoint: `ws://127.0.0.1:8765/ws`

## 2. Install the Chrome extension

1. Open `chrome://extensions`
2. Enable `Developer mode`
3. Click `Load unpacked`
4. Select this `extension` directory

## 3. Runtime flow

1. Open the target webpage in your browser.
2. Keep the extension loaded. In popup, you can check connection status, toggle listener on/off, and change WebSocket IP.
3. In the Python terminal, enter a goal, for example:

```text
user> Click the "Sign in" button on this page.
```

4. Python requests DOM state, calls the LLM for one next action, and sends it to the extension.
5. The loop continues until the LLM returns `done`, then the terminal prints the final answer.

## Stability mechanisms

- WebSocket is kept in an `offscreen document`, so MV3 service worker sleep does not break the bridge.
- Heartbeat + auto re-register: `ping/pong` and periodic `register`, with automatic reconnect on disconnect.
- Idempotent deduplication using `request_id`: repeated requests do not re-run browser actions.
- In-page planning overlay displays goal/step/reason/result and survives page refresh.
- Listener switch in popup can fully disable command listening (and stop reconnect) when turned off.

## Core message protocol

- Python -> Extension
  - `{"type":"get_dom_snapshot","request_id":"..."}`
  - `{"type":"execute_action","request_id":"...","action":{...}}`
- Extension -> Python
  - `{"type":"command_result","request_id":"...","ok":true,"result":{...}}`

## Supported actions

- `navigate` (`url`)
- `back`
- `forward`
- `click` (`selector` or `text`)
- `fill` (`selector`, `value`)
- `scroll` (`top`)
- `done` (LLM output only, not sent to extension)

## Notes

- This is a bridge prototype for “ESP32 controlling a browser”. You can move the same RPC protocol from `esp32_sim_server.py` to real ESP32 firmware later.
- Some websites have CSP/cross-origin constraints that may affect DOM extraction or action reliability.
