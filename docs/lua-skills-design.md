# Lua Skills: Executable, Agent-Authorable Skills for MimiClaw

Design document — v0.1, targeting branch `feat/lua-skills`

## 1. Goal and Non-Goals

Today a MimiClaw skill is a Markdown file: instructions the model reads, with
behavior coming entirely from the fixed C tool set. This design adds a second
skill tier — **scripted skills** — where a skill carries Lua code executed by
an embedded interpreter, and, critically, where **the agent itself can author,
test, and register new scripted skills at runtime** through its existing file
tools. The device gains new behavior without reflashing, and the model gains
the ability to build its own tools.

Non-goals for v1: async/background script jobs (ESP-Claw's cap_lua has these;
we defer), loadable native code, script access to networking, and any change
to the existing Markdown skill path — plain `.md` skills keep working
unchanged.

Why Lua: ANSI C, no OS assumptions, ~200 KB flash footprint, a bytecode
compiler that reports line-precise errors (which the generation loop needs),
and an instruction-count hook that makes preemptive timeouts possible without
threads.

## 2. Skill Format v2

Two layouts coexist under `/spiffs/skills/` (SPIFFS is flat; slashes in names
simulate directories — mind the 64-byte object-name limit, so skill names
should stay under ~20 characters):

```
skills/<name>.md                  # tier L0: prompt-only skill (unchanged)
skills/<name>/SKILL.md            # tier L2: scripted skill manifest
skills/<name>/scripts/main.lua    # entry script
```

`SKILL.md` for a scripted skill stays loader-compatible (first line `# Title`,
description paragraph up to the first blank line — the existing summary
parser needs no change beyond matching the new path pattern) and then adds
structured sections:

```markdown
# LED Breathe

Make an LED breathe on a chosen pin with a chosen period.

## Script
- entry: scripts/main.lua
- args: {"pin": {"type": "integer"}, "period_ms": {"type": "integer", "default": 2000},
         "cycles": {"type": "integer", "default": 10, "max": 100}}
- run with: lua_run_script path=/spiffs/skills/led-breathe/scripts/main.lua args=<json>

## Origin
generated | packaged
```

The args schema lives in prose the model reads on demand (progressive
disclosure, same as today); the firmware does not parse it — argument
validation happens inside the script via the `args` library (§3.3), which
keeps the C surface minimal.

## 3. Runtime

### 3.1 Interpreter component

`components/lua/` vendors Lua 5.4. One `lua_State` per execution, created and
destroyed per call (no persistent interpreter state between calls — skill
state that must persist goes through the `storage` module to SPIFFS).
Interpreter heap comes from a custom `lua_Alloc` backed by PSRAM with a hard
budget (default 64 KB, config `MIMI_LUA_HEAP_MAX`); allocation beyond the
budget fails the script, not the system.

### 3.2 Tools

Two new registry tools:

- `lua_run_script(path, args_json, timeout_ms?)` — execute with sandbox
  limits; returns the script's return value serialized to JSON, or a
  structured error `{line, message}` on failure.
- `lua_check(path)` — compile only (`luaL_loadfile`, never executed); returns
  OK or `{line, message}`. This is the cheap validator the generation loop
  leans on.

Timeouts use `lua_sethook` with an instruction-count hook checking elapsed
time every N instructions; the default budget (10 s) is far below any task
watchdog window, and the hook aborts the script with a catchable error rather
than killing the task. Tool output obeys the per-tool output budget, with one
rule specific to scripts: **error payloads are never truncated before result
payloads** — a compiler error with its line number is worth more to the
authoring loop than any amount of successful output.

### 3.3 Sandbox: module allowlist

The standard `io`, `os`, `require`, and `load`-from-string are not exposed.
Scripts see exactly the registered modules:

| Module | Backing | Notes |
|---|---|---|
| `gpio` | existing `gpio_policy` path | the allowlist is enforced in C; Lua cannot widen it |
| `storage` | SPIFFS file ops | paths restricted to `/spiffs/`, same rule as the file tools |
| `timer` | `esp_timer` / `vTaskDelay` | delays count against the script timeout |
| `json` | cJSON wrapper | encode/decode |
| `args` | arg parsing/validation | schema-checked access to `args_json`, ESP-Claw-style |
| `log` | `ESP_LOG*` | tagged `lua:<skill>` |

Every physical side effect thus flows through the same deterministic policy
layer as the C tools — the script tier adds expressiveness, not privilege.

## 4. Skill Generation: the Agent Authors Its Own Skills

This is the part specific to MimiClaw. ESP-Claw's packaged skills are written
by humans; here the primary author is the model, at runtime, on the device,
through tools it already has. Four pieces make first-try authorship viable
within a 4096-token output budget and a 16 KB context budget:

### 4.1 On-device API reference

Each Lua module ships a compact reference at `/spiffs/lua-api/<module>.md`
(one page each: functions, arguments, return values, error behavior, one
example). These are **not** injected into the system prompt; the authoring
meta-skill instructs the model to `read_file` only the modules it is about to
use. This is the same progressive-disclosure economics as skills themselves —
API docs cost zero tokens until the moment of authorship.

### 4.2 The `skill-creator-lua` meta-skill

A Markdown skill that scripts the authoring procedure:

1. Decide the tier: pure instructions → write a plain `.md` skill (L0);
   loops / computation / hardware sequencing / anything a prompt cannot do
   deterministically → scripted skill (L2).
2. `read_file` the API docs for the modules needed.
3. Write `skills/<name>/scripts/main.lua` (template provided in the
   meta-skill: header comment, `args` schema declaration, main body,
   explicit `return`).
4. `lua_check` the script; on error, fix the reported line and re-check.
5. Test-run via `lua_run_script` with safe arguments (for GPIO skills:
   test on the status LED pin, never on an unlisted pin — the policy layer
   rejects those anyway).
6. Write `SKILL.md` (template provided), marking `## Origin` as `generated`.
7. Confirm the skill appears via `list_dir` and report the summary line the
   user will see.

The write → check → test → fix cycle runs inside ordinary ReAct iterations;
no new control flow is added to the loop. What the firmware contributes is
error quality: compile and runtime errors come back compact, line-numbered,
and untruncated, so each repair iteration costs a few hundred bytes of
context, not kilobytes.

### 4.3 Trust marking and the actuation gate

Skills carry `## Origin: generated | packaged`. A config option
(`MIMI_LUA_GENERATED_CONFIRM`, default on) requires that the first
`lua_run_script` of a *generated* skill that touches `gpio` be echoed to the
user for confirmation before execution; subsequent runs are unrestricted.
This is a one-time human checkpoint at the trust boundary — cheap, and it
converts "the model wrote code that moves hardware" from a silent event into
a visible one.

### 4.4 Prompt contract

The system prompt gains ~6 lines: the existence of scripted skills, the
decision rule (prompt vs script), and the instruction to use the
`skill-creator-lua` meta-skill when the user asks for a new capability.
Everything else stays out of the prompt and inside the meta-skill and API
docs, keeping the context budget intact.

## 5. Loader Changes

`skill_loader_build_summary` additionally matches `skills/*/SKILL.md`; the
summary line format is unchanged (title, description, read-with path). Plain
`.md` and scripted skills are indistinguishable in the summary — the model
discovers the script by reading `SKILL.md`, which is exactly the existing
interaction pattern.

## 6. Implementation Phases

| PR | Scope | Demo / exit criterion |
|---|---|---|
| 1 | `components/lua`, `lua_run_script` + `lua_check`, sandbox (allowlist, heap cap, timeout hook), `gpio`/`json`/`args`/`log` modules | packaged LED-blink script runs; runaway loop killed at timeout; off-policy pin rejected from Lua |
| 2 | Skill format v2 + loader matching, `storage`/`timer` modules | scripted skill discovered in summary and executed end-to-end |
| 3 | `/spiffs/lua-api/` docs, `skill-creator-lua` meta-skill, origin marking + actuation gate | **the demo**: user asks "make the LED breathe" in Telegram; the agent authors, checks, tests, and registers the skill unaided |

Phase 3's demo is the acceptance test for the whole design: if the model
cannot reliably author a working skill through this pipeline, the API docs or
the meta-skill need work — measured by first-try success rate and repair
iteration count, which the `lua_check`/`lua_run_script` split makes directly
observable in logs.
