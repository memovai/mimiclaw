#!/usr/bin/env python3
import asyncio
import json
import os
import uuid
from dataclasses import dataclass
from typing import Any, Dict, Optional

import websockets
from websockets.exceptions import ConnectionClosed
from openai import OpenAI


HOST = "127.0.0.1"
PORT = 8765
PATH = "/ws"
DEFAULT_MODEL = os.getenv("OPENAI_MODEL", "gpt-5-nano")


SYSTEM_PROMPT = """You are the decision engine for a browser automation assistant.
Given a user goal and current DOM snapshot, return ONE next action in strict JSON.
Allowed actions:
- navigate: requires url
- back: no extra fields
- forward: no extra fields
- click: selector or text
- fill: selector and value
- scroll: top integer
- done: provide answer to user
Rules:
1. Prefer safe, minimal actions.
2. If needed target cannot be found, prefer navigate/click to make the target visible first; use done only when truly blocked.
3. Return only JSON with keys: action, selector, text, url, value, top, answer, reason.
4. For posting on X/Twitter, prefer sequence: navigate to x.com -> open composer -> fill -> click post.
5. Do not emit fill/click before composer/input is visible in DOM interactiveElements.
6. For X/Twitter posts, keep text within 200 characters.
7. fill action must replace existing text in the target input/editor, not append.
"""


@dataclass
class PendingRequest:
  future: asyncio.Future


class BridgeState:
  def __init__(self) -> None:
    self.extension_ws: Optional[Any] = None
    self.pending: Dict[str, PendingRequest] = {}

  def set_extension(self, ws: Any) -> None:
    self.extension_ws = ws

  def clear_extension(self, ws: Any) -> None:
    if self.extension_ws is ws:
      self.extension_ws = None

  async def rpc(self, payload: Dict[str, Any], timeout: float = 15.0, retries: int = 2) -> Dict[str, Any]:
    if self.extension_ws is None:
      raise RuntimeError("No extension connected. Load extension and keep service worker active.")

    request_id = str(uuid.uuid4())
    payload = dict(payload)
    payload["request_id"] = request_id

    loop = asyncio.get_running_loop()
    fut: asyncio.Future = loop.create_future()
    self.pending[request_id] = PendingRequest(future=fut)

    try:
      for attempt in range(1, retries + 2):
        if self.extension_ws is None:
          raise RuntimeError("Extension disconnected while waiting for command result.")

        await self.extension_ws.send(json.dumps(payload, ensure_ascii=False))
        try:
          result = await asyncio.wait_for(asyncio.shield(fut), timeout=timeout)
          return result
        except asyncio.TimeoutError:
          if attempt >= retries + 1:
            raise TimeoutError(f"RPC timeout request_id={request_id} after {attempt} attempts") from None
          print(f"[server] rpc retry request_id={request_id} attempt={attempt + 1}")
    finally:
      self.pending.pop(request_id, None)

  def resolve(self, request_id: str, message: Dict[str, Any]) -> None:
    pending = self.pending.get(request_id)
    if not pending or pending.future.done():
      return
    pending.future.set_result(message)

  async def notify(self, payload: Dict[str, Any]) -> None:
    if self.extension_ws is None:
      return
    try:
      await self.extension_ws.send(json.dumps(payload, ensure_ascii=False))
    except Exception:
      return


class Esp32Agent:
  def __init__(self, bridge: BridgeState) -> None:
    self.bridge = bridge
    api_key = os.getenv("OPENAI_API_KEY", "").strip()
    if not api_key:
      raise RuntimeError("OPENAI_API_KEY is required")
    self.model = DEFAULT_MODEL
    self.client = OpenAI(api_key=api_key)

  def _ask_llm(self, user_goal: str, dom_snapshot: Dict[str, Any]) -> Dict[str, Any]:
    dom_json = json.dumps(dom_snapshot, ensure_ascii=False)
    prompt = (
      "User goal:\n"
      f"{user_goal}\n\n"
      "Current DOM snapshot (JSON):\n"
      f"{dom_json}\n"
    )

    resp = self.client.chat.completions.create(
      model=self.model,
      response_format={"type": "json_object"},
      messages=[
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": prompt},
      ],
    )

    content = resp.choices[0].message.content or "{}"
    try:
      return json.loads(content)
    except json.JSONDecodeError:
      return {
        "action": "done",
        "answer": f"LLM output is not valid JSON: {content}",
        "reason": "invalid_json"
      }

  def _is_blocked_done(self, answer: str, reason: str) -> bool:
    text = f"{answer} {reason}".lower()
    keywords = [
      "unable",
      "cannot",
      "can't",
      "could not",
      "couldn't",
      "not found",
      "not locate",
      "cannot identify",
      "selector",
      "input element",
    ]
    return any(k in text for k in keywords)

  def _is_twitter_url(self, url: str) -> bool:
    u = (url or "").lower()
    return "://x.com" in u or "://twitter.com" in u

  def _is_twitter_publish_click(
    self,
    action_payload: Dict[str, Any],
    action_result: Dict[str, Any],
    dom_snapshot: Dict[str, Any],
  ) -> bool:
    if action_payload.get("name") != "click":
      return False
    if not self._is_twitter_url(str(dom_snapshot.get("url", ""))):
      return False

    selector_text = f"{action_payload.get('selector', '')} {action_result.get('selector', '')}".lower()
    button_text = f"{action_payload.get('text', '')} {action_result.get('text', '')}".strip().lower()

    if "tweetbutton" in selector_text:
      return True
    return button_text in {"post", "tweet", "发布", "發佈"}

  async def _confirm_twitter_posted(self) -> bool:
    # Best effort confirmation: after a successful click on Post button,
    # composer usually closes or input becomes empty.
    for _ in range(2):
      await asyncio.sleep(0.8)
      dom_rsp = await self.bridge.rpc(
        {"type": "get_dom_snapshot", "maxText": 2400, "maxElements": 60},
        timeout=20,
        retries=1,
      )
      if not dom_rsp.get("ok"):
        continue
      snap = dom_rsp.get("result", {})
      if not self._is_twitter_url(str(snap.get("url", ""))):
        continue
      compose = snap.get("twitterCompose") or {}
      has_composer = bool(compose.get("hasComposer"))
      draft_len = int(compose.get("draftLength") or 0)
      if (not has_composer) or draft_len == 0:
        return True
      txt = str(snap.get("textSnippet", "")).lower()
      if "your post was sent" in txt or "posted" in txt or "已发布" in txt or "已發佈" in txt:
        return True
    return False

  async def _confirm_twitter_fill_ready(self) -> bool:
    for _ in range(2):
      await asyncio.sleep(0.5)
      dom_rsp = await self.bridge.rpc(
        {"type": "get_dom_snapshot", "maxText": 2400, "maxElements": 60},
        timeout=20,
        retries=1,
      )
      if not dom_rsp.get("ok"):
        continue
      snap = dom_rsp.get("result", {})
      if not self._is_twitter_url(str(snap.get("url", ""))):
        return True
      compose = snap.get("twitterCompose") or {}
      draft_len = int(compose.get("draftLength") or 0)
      post_enabled = bool(compose.get("postButtonEnabled"))
      if draft_len > 0 and post_enabled:
        return True
    return False

  async def run_goal(self, user_goal: str, max_steps: int = 14) -> str:
    print(f"\n[agent] New goal: {user_goal}")
    await self.bridge.notify({"type": "agent_status", "event": "goal", "goal": user_goal})
    last_dom_key = ""
    stagnant_rounds = 0
    last_action_sig = ""
    same_action_rounds = 0
    action_seen: Dict[str, int] = {}
    max_action_attempts = 5
    done_backtrack_attempts = 0
    max_done_backtracks = 2

    for step in range(1, max_steps + 1):
      dom_rsp = None
      last_dom_error = ""
      for attempt in range(1, 4):
        dom_rsp = await self.bridge.rpc(
          {"type": "get_dom_snapshot", "maxText": 3500, "maxElements": 80},
          timeout=25,
          retries=2,
        )
        if dom_rsp.get("ok"):
          break
        last_dom_error = dom_rsp.get("error", "unknown_error")
        print(f"[agent] dom_retry step={step} attempt={attempt} error={last_dom_error}")
        await asyncio.sleep(0.6)

      if not dom_rsp.get("ok"):
        err = f"DOM capture failed: {last_dom_error or dom_rsp.get('error', 'unknown_error')}"
        await self.bridge.notify({"type": "agent_status", "event": "done", "ok": False, "error": err})
        return err

      dom_snapshot = dom_rsp.get("result", {})
      dom_key = "|".join(
        [
          str(dom_snapshot.get("url", "")),
          str(dom_snapshot.get("title", "")),
          str(dom_snapshot.get("textSnippet", ""))[:240],
        ]
      )
      if dom_key == last_dom_key:
        stagnant_rounds += 1
      else:
        stagnant_rounds = 0
      last_dom_key = dom_key

      plan = await asyncio.to_thread(self._ask_llm, user_goal, dom_snapshot)

      action = (plan.get("action") or "done").strip().lower()
      print(f"[agent] step={step} action={action} reason={plan.get('reason', '')}")
      await self.bridge.notify({
        "type": "agent_status",
        "event": "step",
        "step": step,
        "action": action,
        "reason": plan.get("reason", ""),
      })

      if action == "done":
        answer = plan.get("answer") or "Task finished."
        reason_text = str(plan.get("reason") or "")

        # If LLM exits because it can't find elements, roll back one step and retry.
        if self._is_blocked_done(answer, reason_text) and done_backtrack_attempts < max_done_backtracks and step > 1:
          done_backtrack_attempts += 1
          back_reason = f"LLM done due to element-missing; auto backtrack {done_backtrack_attempts}/{max_done_backtracks}"
          print(f"[agent] {back_reason}")
          await self.bridge.notify({
            "type": "agent_status",
            "event": "step",
            "step": step,
            "action": "backtrack",
            "reason": back_reason,
          })

          back_rsp = await self.bridge.rpc(
            {"type": "execute_action", "action": {"name": "back"}},
            timeout=30,
            retries=2,
          )
          if not back_rsp.get("ok"):
            err = f"Backtrack failed: {back_rsp.get('error', 'unknown_error')}"
            await self.bridge.notify({"type": "agent_status", "event": "done", "ok": False, "error": err})
            return err

          back_result = back_rsp.get("result", {})
          if not back_result.get("ok", True):
            err = f"Backtrack browser action error: {back_result.get('error', 'unknown_error')}"
            await self.bridge.notify({"type": "agent_status", "event": "done", "ok": False, "error": err})
            return err

          await asyncio.sleep(1.0)
          continue

        await self.bridge.notify({"type": "agent_status", "event": "done", "ok": True, "answer": answer})
        return answer

      if action not in {"navigate", "back", "forward", "click", "fill", "scroll"}:
        err = f"Unsupported action from LLM: {action}"
        await self.bridge.notify({"type": "agent_status", "event": "done", "ok": False, "error": err})
        return err

      action_payload: Dict[str, Any] = {"name": action}
      for key in ("selector", "text", "url", "value", "top"):
        if key in plan and plan.get(key) not in (None, ""):
          action_payload[key] = plan[key]

      action_sig = json.dumps(
        {
          "name": action_payload.get("name", ""),
          "selector": str(action_payload.get("selector", ""))[:120],
          "text": str(action_payload.get("text", ""))[:120],
          "url": str(action_payload.get("url", ""))[:180],
          "value": str(action_payload.get("value", ""))[:80],
          "top": action_payload.get("top", ""),
        },
        ensure_ascii=False,
        sort_keys=True,
      )

      if action_sig == last_action_sig:
        same_action_rounds += 1
      else:
        same_action_rounds = 1
      last_action_sig = action_sig

      action_seen[action_sig] = action_seen.get(action_sig, 0) + 1

      if same_action_rounds >= 3:
        err = f"Stopped to avoid loop: same action repeated {same_action_rounds} times."
        await self.bridge.notify({"type": "agent_status", "event": "done", "ok": False, "error": err})
        return err

      if action_seen[action_sig] >= 5:
        err = "Stopped to avoid loop: same action pattern occurred too many times."
        await self.bridge.notify({"type": "agent_status", "event": "done", "ok": False, "error": err})
        return err

      if stagnant_rounds >= 3 and action in {"navigate", "back", "forward", "click"}:
        err = "Stopped to avoid loop: page state unchanged across multiple steps."
        await self.bridge.notify({"type": "agent_status", "event": "done", "ok": False, "error": err})
        return err

      final_action_error = ""
      action_ok = False
      for action_try in range(1, max_action_attempts + 1):
        cmd_rsp = await self.bridge.rpc(
          {"type": "execute_action", "action": action_payload},
          timeout=30,
          retries=2,
        )
        if not cmd_rsp.get("ok"):
          final_action_error = f"Action execution failed: {cmd_rsp.get('error', 'unknown_error')}"
        else:
          result = cmd_rsp.get("result", {})
          if not result.get("ok", True):
            final_action_error = f"Browser action error: {result.get('error', 'unknown_error')}"
          else:
            action_ok = True
            if action_payload.get("name") == "fill" and self._is_twitter_url(str(dom_snapshot.get("url", ""))):
              fill_ready = await self._confirm_twitter_fill_ready()
              if not fill_ready:
                action_ok = False
                final_action_error = "Twitter fill not applied to editor state (post button still disabled)."
                if action_try < max_action_attempts:
                  print(f"[agent] action_retry step={step} action=fill attempt={action_try + 1} reason={final_action_error}")
                  await self.bridge.notify({
                    "type": "agent_status",
                    "event": "step",
                    "step": step,
                    "action": action,
                    "reason": f"Retry {action_try + 1}/{max_action_attempts}: {final_action_error}",
                  })
                  await asyncio.sleep(0.8)
                  continue
            if self._is_twitter_publish_click(action_payload, result, dom_snapshot):
              confirmed = await self._confirm_twitter_posted()
              answer = (
                "Post published on X successfully."
                if confirmed
                else "Post button clicked on X. Submission sent; ending task to avoid duplicate posting."
              )
              await self.bridge.notify({"type": "agent_status", "event": "done", "ok": True, "answer": answer})
              return answer
            break

        if action_try < max_action_attempts:
          print(f"[agent] action_retry step={step} action={action} attempt={action_try + 1} reason={final_action_error}")
          await self.bridge.notify({
            "type": "agent_status",
            "event": "step",
            "step": step,
            "action": action,
            "reason": f"Retry {action_try + 1}/{max_action_attempts}: {final_action_error}",
          })
          await asyncio.sleep(0.8)

      if not action_ok:
        # Auto-recovery for X/Twitter composer opening: when click target is not found,
        # directly navigate to compose URL and continue next planning step.
        if (
          action == "click"
          and "click target not found" in (final_action_error or "").lower()
          and self._is_twitter_url(str(dom_snapshot.get("url", "")))
        ):
          nav_rsp = await self.bridge.rpc(
            {"type": "execute_action", "action": {"name": "navigate", "url": "https://x.com/compose/post"}},
            timeout=30,
            retries=2,
          )
          if nav_rsp.get("ok") and (nav_rsp.get("result", {}) or {}).get("ok", True):
            await asyncio.sleep(0.8)
            continue
        await self.bridge.notify({"type": "agent_status", "event": "done", "ok": False, "error": final_action_error})
        return final_action_error

      await asyncio.sleep(1.0)

    err = "Stopped after max steps without done action."
    await self.bridge.notify({"type": "agent_status", "event": "done", "ok": False, "error": err})
    return err


async def ws_handler(bridge: BridgeState, websocket: Any, path: str) -> None:
  print(f"[server] client connected path={path or '/'}")
  registered_once = False
  try:
    async for raw in websocket:
      try:
        data = json.loads(raw)
      except json.JSONDecodeError:
        continue

      mtype = data.get("type")
      if mtype == "register" and data.get("role") == "extension":
        bridge.set_extension(websocket)
        if not registered_once:
          print("[server] extension registered")
          registered_once = True
        else:
          print("[server] extension re-registered")
        await websocket.send(json.dumps({"type": "register_ack", "ts": int(asyncio.get_running_loop().time() * 1000)}))
        continue

      if mtype == "ping":
        await websocket.send(json.dumps({"type": "pong", "ts": data.get("ts")}))
        continue

      if mtype == "command_result":
        request_id = data.get("request_id")
        if request_id:
          bridge.resolve(request_id, data)
        continue
  except ConnectionClosed as exc:
    # Client refresh/reconnect will close the socket; treat it as normal lifecycle.
    print(f"[server] client closed code={exc.code} reason={exc.reason or 'none'}")
  except Exception as exc:
    print(f"[server] ws_handler error: {exc}")
  finally:
    bridge.clear_extension(websocket)
    print("[server] client disconnected")


async def input_loop(agent: Esp32Agent) -> None:
  print("\n=== ESP32 Simulator Ready ===")
  print("Type a goal and press Enter. Type 'exit' to quit.\n")

  while True:
    goal = await asyncio.to_thread(input, "user> ")
    goal = goal.strip()
    if not goal:
      continue
    if goal.lower() in {"exit", "quit"}:
      break

    try:
      answer = await agent.run_goal(goal)
      print(f"assistant> {answer}\n")
    except Exception as exc:
      print(f"assistant> Error: {exc}\n")


async def main() -> None:
  bridge = BridgeState()

  async def handler_compat(websocket: Any, path: Optional[str] = None) -> None:
    actual_path = path or getattr(websocket, "path", "")
    # websockets>=13 stores path in websocket.request.path
    if not actual_path:
      request = getattr(websocket, "request", None)
      actual_path = getattr(request, "path", "") if request else ""
    await ws_handler(bridge, websocket, actual_path)

  server = await websockets.serve(handler_compat, HOST, PORT)
  print(f"[server] ws://{HOST}:{PORT}{PATH}")

  agent = Esp32Agent(bridge)

  try:
    await input_loop(agent)
  finally:
    server.close()
    await server.wait_closed()


if __name__ == "__main__":
  asyncio.run(main())
