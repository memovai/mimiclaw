# SpecClaw: Guarded Speculative Execution for MCU-Hosted LLM Agent Loops

**Paper proposal + implementation plan** · Draft v0.2 · 2026-07-13
Working title: *"Speculate Locally, Reason Remotely: Guarded Speculative Execution for Agent Loops on Microcontrollers"*

---

## 1. One-sentence pitch

A single cloud LLM call drafts the next k steps as `(action, guard envelope, timeout)` tuples; the MCU verifies and commits them step by step using deterministic C code (guarded commit), and only calls back to the cloud with a compact state snapshot when a prediction diverges — dividing cloud calls per task by the mean acceptance length L, while guaranteeing that irreversible physical actions are never speculatively mis-executed.

## 2. Motivation

### 2.1 The cost structure of ReAct on an MCU

Standard ReAct issues one cloud call per step. On an ESP32-S3, every call carries a **fixed overhead independent of token count**:

- radio wake-up + WiFi transmission window (the dominant energy term)
- TLS handshake ~1–3 s (without connection reuse)
- TTFB + cloud inference latency ~1–5 s

An n-step task pays this fixed overhead n times. Server-side agent research counts only tokens and never accounts for this term; on a battery-powered device it dominates.

### 2.2 Three assumptions of server-side speculation that our setting inverts

A cluster of server-side speculative-agent papers emerged in 2025–2026 (Dynamic Speculative Agent Planning 2509.01920, SPAgent 2511.20048, Sherlock 2511.00330, B-PASTE 2604.16469, IdleSpec 2605.22154, etc.). Each of their shared assumptions is inverted in our setting:

| | Server-side speculation | This work (MCU + physical actuators) |
|---|---|---|
| Verifier | target LLM / draft–target agreement | **the physical world + deterministic guard code** (zero inference) |
| Objective | latency overlap (hide inference time) | **the number of calls itself** (energy, cost, offline resilience) |
| Action semantics | execute eagerly, discard freely (search/prefetch) | **irreversible** (relays/motors): guarded commit, never eager execution |

The third row spawns a new problem: speculation depth is bounded by an **irreversibility horizon** — a guard false-accept (guard passes but the world has semantically diverged) means a physical action was wrongly taken and cannot be undone. We define this failure mode as **silent divergence** and measure it as a first-class metric.

### 2.3 A side benefit: disconnection resilience

The speculation script lives on the device, so the MCU keeps executing the verified prefix during network outages; cloud-orchestrated architectures stall completely. This differentiates the hybrid-architecture comparison (part of our evaluation).

## 3. Related work and positioning

| Category | Representative work | Relation to this work |
|---|---|---|
| Server-side speculative agents | 2509.01920, 2511.20048, 2511.00330, 2603.18897, 2604.16469, 2605.22154, 2606.02483 | Conceptual predecessors; all server-side, latency-oriented, with discardable actions. Cited and differentiated via the three inversions in §2.2 |
| Plan-then-execute | ReWOO (2305.18323), LLM Compiler (2312.04511) | One-shot full planning but **open-loop** (no per-step verification); serves as baseline B2 |
| Device–cloud closed loop | EcoAgent (2505.05440, AAAI'26) | Closest precedent: full plan + per-step expectation + cloud replan on failure. Key differences: its device-side verification requires a 2B VLM (phone-class); no model of any size runs on an MCU → we contribute a **structured guard language + deterministic evaluator** instead. Also: fixed (non-tunable) depth, no energy/bytes metrics, no irreversibility handling |
| LLM→MCU protocols | DCP (2605.26159) | The opposite architecture (loop on host, MCU as dumb endpoint); we borrow its capability-scoping / range-check ideas into the guard language |
| MCU agent OSS | ESP-Claw, WireClaw, zclaw, MimiClaw | Engineering precedents; no papers, no speculation mechanism; acknowledged in related work + MimiClaw is our experimental platform |
| State-based context | MEM1 (2506.15841), StateAct (2410.02810), 2606.14945 | The repredict payload uses a compact state block; cited as token-level precedents. StateAct found JSON-formatted state hurts accuracy → our state block uses plain structured text |

**Novelty claim** (three legs, all required): ① guarded-commit speculation semantics for irreversible actions + the definition and measurement of silent divergence; ② a structured guard language requiring no device-side model (LLM-generated, C-evaluated); ③ evaluation on a real MCU with joules/bytes/call-count metrics, including the depth-vs-stochasticity relationship and disconnection resilience.

**Explicitly out of scope for v1** (kept for follow-up work, to avoid dilution): upgrading the speculation artifact to branching trees / generated FSMs; adaptive depth beyond the single simplest variant (§6.6).

## 4. Mechanism design

### 4.1 Speculation Script protocol

Each cloud call returns (enforced via structured output):

```json
{
  "state_update": "relay=on; last_temp=27.3; goal_phase=cooling",
  "script": [
    {
      "step": 1,
      "action": {"tool": "gpio", "args": {"pin": 5, "level": 1}},
      "irreversible": false,
      "guard": {
        "pre":  [{"var": "temp_c", "op": "gt", "val": 26.0}],
        "post": [{"var": "temp_c", "op": "lt", "val": 26.5, "within_s": 120},
                 {"var": "relay_current_ma", "op": "in", "lo": 80, "hi": 200, "within_s": 2}]
      },
      "on_fail": "replan"
    }
  ],
  "done_when": [{"var": "temp_c", "op": "lt", "val": 25.0}]
}
```

Guard language (deliberately minimal in v1): numeric comparisons `gt/lt/in`, discrete equality `eq`, rate-of-change `delta_lt`, time bounds `within_s`, AND composition only (no OR). The limited expressiveness is a feature, not a bug — decidable, evaluable in ~100 lines of C, statically auditable.

### 4.2 Guarded-commit execution semantics

```
for step in script:
    if not eval(step.guard.pre):        → mispredict, goto REPLAN
    if step.irreversible and depth_since_last_verify > D_irrev:
                                        → forced cloud confirmation (safety valve)
    execute(step.action)                 # through the existing gpio_policy allowlist
    if not eval_within(step.guard.post): → mispredict, goto REPLAN
    commit(step)                         # append to execution log (SPIFFS, survives power loss)
REPLAN:
    snapshot = compact_state()           # §4.3
    call_cloud(snapshot)                 # one call, returns a fresh script
```

Key invariants: **an action executes only after its pre-guard passes, and irreversible actions are additionally bounded by the depth valve D_irrev**. All actions still pass through the existing `gpio_policy` allowlist — the speculation layer never bypasses the safety layer.

### 4.3 Repredict payload: compact state block

On mispredict, the device does not upload the full transcript; it uploads a fixed-size state block (plain structured text, not JSON):

```
GOAL: keep room below 25C using relay-controlled fan
STATE: temp=27.9 relay=on rssi=-61 heap_free=118k
COMMITTED: [1:gpio(5,1) OK] [2:wait OK]
FAILED_AT: step 3, guard post temp_c<26.5 within 120s, actual 27.9
FACTS: sensor2 reported stuck value twice; user requires quiet mode after 22:00
```

Uplink volume is O(1) (~300–500 B, vs. a full transcript of several KB growing with step count).

### 4.4 Terminology (for the paper)

- **Acceptance length L**: the number of consecutive steps successfully verified and committed after one cloud call (the systems-level analogue of acceptance length in speculative decoding)
- **Silent divergence**: a committed step whose guard passed while the environment ground truth had already diverged from the task semantics (guard false-accept)
- **Guard false-reject**: the environment has not diverged but an overly tight guard triggers a replan (one wasted call)
- **D_irrev**: the maximum number of irreversible actions allowed to execute speculatively since the last cloud confirmation

## 5. Research questions and hypotheses

- **RQ1 (benefit)**: At equal success rate, how much does guarded speculation reduce cloud calls / energy / median latency versus step-by-step ReAct?
  *H1: calls drop to 1/L (expected L∈[3,8]); joules per task drop ≥60% (fixed overhead dominates); local steps at ~ms latency produce a bimodal latency distribution.*
- **RQ2 (depth vs. stochasticity)**: How does the optimal speculation depth k vary with environment stochasticity?
  *H2: in deterministic environments L→k (deeper is better); as noise/fault injection increases, L collapses and a crossover exists where k>1 yields no benefit — producing a quantitative characterization of "how deep is worth speculating in which environments."*
- **RQ3 (guard quality)**: What does the false-accept (→silent divergence) vs. false-reject trade-off curve of LLM-generated guard envelopes look like? How do envelope tightness and guard density (post-conditions per step) shift it?
  *H3: a clear ROC-shaped trade-off exists; LLM-default envelopes are too loose (nonzero silent divergence), and prompting for "at least one observable post-condition per action" suppresses silent divergence at a small false-reject cost.*
- **RQ4 (disconnection resilience)**: Under injected network outages of 10 s / 60 s / 600 s, how much higher is task survival for speculative execution versus step-by-step ReAct and cloud-orchestrated baselines?
  *H4: tasks complete normally when the outage falls within the verified prefix; the survival gap widens with speculation depth.*

## 6. Experimental design

### 6.1 Substrate: virtual environment primary, physical bench secondary

The platform is MimiClaw — an open-source agent firmware already running stably on ESP32-S3 (full inventory of existing capabilities and gaps in Part II). All methods (including baselines) are implemented as build configurations of the same firmware, eliminating implementation-difference confounds.

- **Primary substrate**: a host-side Python environment simulator, attached as tools through MimiClaw's existing WebSocket gateway (the MCU runs real firmware and makes real cloud API calls; only sensors/actuators are simulated). Rationale: controllable stochasticity injection, ground truth availability (silent divergence can only be judged against ground truth), reproducibility, and large-batch runs.
- **Secondary substrate**: one physical bench (ESP32-S3 + temperature sensor + relay/fan + INA226 current monitor) running a task subset for measured energy numbers and a case study, preempting the "it's all simulation" review.

### 6.2 Task suite (v1: 18 tasks)

A grid spanned by three dimensions, 2–3 tasks per cell:

- **Horizon**: short (3–5 steps) / medium (8–12) / long (20+)
- **Environment predictability**: deterministic / noisy (Gaussian sensor noise + occasional outliers) / adversarial (injected I2C errors, actuator failures, state jumps)
- **Irreversibility**: read-only / reversible actuation (lights) / irreversible actuation (simulated irrigation water release, one-shot latches)

Example tasks: thermostat control, timed irrigation (irreversible water volume), multi-sensor fault diagnosis, actuator sequencing (power-up order with per-step confirmation), fan scheduling under a night-quiet constraint (tests FACTS retention). Each task ships a ground-truth judge function (simulator side).

### 6.3 Baselines

| ID | Name | Description |
|---|---|---|
| B1 | ReAct | One cloud call per step, no expectations (the existing MimiClaw agent loop) |
| B1+ | ReAct + prompt caching | Same, with Anthropic prefix caching enabled (preempts "caching already solved cost") |
| B2 | Plan-then-execute (ReWOO-style) | One full plan, executed open-loop to the end, no guards. Expected to collapse in noisy/adversarial cells, showcasing the value of guards |
| B3 | Full plan + naive equality guards (an MCU-feasible adaptation of EcoAgent) | Depth = full length, guards limited to exact equality/existence checks, any failure triggers whole-plan replanning |
| Ours | SpecClaw | Tunable depth k + envelope guard language + compact-state repredict + D_irrev |

### 6.4 Metrics

Success rate (ground-truth judged); mean L; cloud calls per task / cloud tokens (input/output separately) / uplink bytes; joules per task (INA226/PPK2, bench subset); per-step latency distribution (expected bimodal; report median + full distribution plot); silent divergence rate; guard false-reject rate; PSRAM high-water mark; disconnection survival rate. ≥10 seeds per configuration, 95% CIs.

### 6.5 Ablations

Guard density (no post-conditions / 1 per step / several per step); envelope tightness (LLM default / prompt-tightened ±10% / ±30%); repredict payload (compact state vs. full transcript); D_irrev ∈ {0,1,3,∞}; model tier (Haiku vs. Sonnet — how much worse are scripts and guards from a weaker model).

### 6.6 Adaptive depth (simplest variant only)

One variant only: prompt the cloud model to decide for itself "how far it dares to speculate and truncate the script early at uncertain points," compared against fixed k. No learned scheduler.

### 6.7 Target figures

1. Bimodal per-step latency histogram (ms-scale local peak vs. s-scale cloud peak)
2. L and net benefit vs. environment stochasticity (the RQ2 crossover curve — **the core figure**)
3. Grouped bars: joules and calls per task (5 methods × 3 stochasticity levels)
4. Guard ROC: silent divergence vs. false-reject (sweeping envelope tightness)
5. Outage duration vs. task survival (4 methods)
6. Success-vs-energy Pareto scatter (all methods, all configurations)

## 7. Contributions (draft claims)

1. The first speculative-execution semantics for agent loops with irreversible physical actions (guarded commit + D_irrev), plus the definition and measurement methodology of the new failure mode, silent divergence
2. A minimal guard language that an LLM can generate and an MCU can evaluate deterministically, replacing device-side model-based verification in device–cloud closed loops
3. Evaluation on a real ESP32-S3 system: measured calls/joules/bytes/latency, the speculation-depth-vs-environment-stochasticity relationship, and disconnection resilience — the first systems data for MCU-hosted agent loops
4. Open source: firmware + environment simulator + task suite (groundwork for a future MCU-AgentBench)

## 8. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Measured L too short (<2), collapsing the benefit story | The fixed-overhead accounting still holds (L=2 already halves handshakes); RQ2 itself sells "when speculation is not worth it" as a finding |
| Unstable structured output (malformed scripts) | Structured-output enforcement + device-side schema validation with reject-and-retry; malformed rate reported as a secondary metric |
| Reviewer: "isn't this just EcoAgent on smaller hardware?" | §3 table answers head-on: guard language replaces the device-side VLM, irreversibility semantics, energy/bytes metrics, depth analysis, disconnection — five deltas |
| Reviewer: "simulated environments are unrealistic" | Bench subset with measured energy + case study; simulator modeled conservatively (noise parameters taken from real sensor datasheets) |
| Scooping (this area moves in six-month waves) | Post an arXiv report right after M1; the speculation × MCU × irreversibility intersection is currently unoccupied |

## 9. Target venues and cadence

Primary targets: SenSys / MobiSys / IPSN (systems track), fallback EWSN; a 4-page version to a HotMobile/HotEdge-class workshop first as a test run and stake in the ground. Post to arXiv immediately after M1. (Check CFPs for exact deadlines.)

---

# Part II · Implementation Plan

## Current state of MimiClaw

This project does not build a platform from scratch — MimiClaw is an agent firmware already running stably on real ESP32-S3 hardware (open source; ESP-Claw/WireClaw/zclaw are peer projects corroborating the architecture's feasibility). The inventory below covers what matters to this project; all numbers come from `main/mimi_config.h`.

### Hardware and system foundation

- **ESP32-S3** (dual-core Xtensa LX7 + PSRAM), 16 MB flash: 2×2 MB OTA partitions + 12 MB SPIFFS
- ESP-IDF + FreeRTOS; large buffers allocated from PSRAM (e.g., the 32 KB LLM streaming buffer)
- Three-tier configuration: build-time `mimi_secrets.h` > NVS (six namespaces: wifi/tg/feishu/llm/proxy/search) > defaults
- Provisioning/config entry points: serial CLI (esp_console REPL) + captive-portal onboarding (open AP `MimiClaw-*`, HTTP 80)

### FreeRTOS task layout

| Task | Core | Prio | Stack | Role |
|---|---|---|---|---|
| Agent loop | 1 | 6 | 24 KB | inbound queue → build context → call LLM → tool iterations → outbound |
| Telegram poller | 0 | 5 | 12 KB | getUpdates long polling (30 s timeout) |
| Feishu channel | 0 | 5 | 12 KB | webhook (port 18790) or polling mode |
| Outbound dispatch | 0 | 5 | 12 KB | routes outgoing messages to Telegram/Feishu/WS |
| Serial CLI | 0 | 3 | 4 KB | configuration REPL |

Message bus: two FreeRTOS queues (inbound/outbound, depth 16) decoupling all channels from the agent.

### Agent loop as it stands (= baseline B1)

- ReAct loop: up to `MIMI_AGENT_MAX_TOOL_ITER`=10 tool iterations per message, up to 4 parallel tool calls per iteration
- History window of 20 messages; sessions persisted to SPIFFS (`/spiffs/sessions/`, 20 messages per session cap)
- context_builder assembly order: SOUL.md (persona) + USER.md + MEMORY.md (long-term memory, `/spiffs/memory/`) + session history + tool schemas; 16 KB context buffer

### LLM access layer

- `llm_proxy` supports the Anthropic messages API and OpenAI-compatible endpoints (default `claude-opus-4-5`, max_tokens 4096), streaming parser, HTTP proxy support
- **Missing**: structured-output enforcement, per-call metering (handshake/TTFB/bytes up-down) — the M1 and M0 work respectively

### Tool layer

- `tool_registry` registration model. Existing tools: GPIO read/write (constrained by the `gpio_policy` allowlist), SPIFFS file read/write, cron job management, get_time, web_search (Tavily)
- **`gpio_policy` is the embryo of the guard layer**: speculative action commits reuse this allowlist path; the spec layer opens no new execution channel

### Autonomy subsystems (an unattended loop already exists)

- cron_service: up to 16 jobs in `cron.json`, 60 s check granularity; a firing job injects a message to the agent
- heartbeat: reads HEARTBEAT.md every 30 min to wake the agent for self-checks
- Together these mean the runtime skeleton for "long-running autonomous agent" already exists; scheduled-task workloads in the experiments need no new mechanism

### Other reusable assets

- WS gateway (port 18789, 4 clients): JSON protocol ready-made — the attachment point for env_sim
- skill_loader: skill files under `/spiffs/skills/` injected into context on demand
- OTA (esp_https_ota + dual partitions): remote firmware/parameter updates between experiment batches
- `components/micropython_embed`: embedded MicroPython runtime (optional asset; v1 does not depend on it)

### Gap list (everything this project must add)

1. Per-call metering instrumentation (M0)
2. Structured-output enforcement + schema validation (M1)
3. All six modules under `main/spec/` (M1–M2)
4. Virtual-environment tool `tool_env` + host-side env_sim (M0/M3)
5. Noise/fault/outage injection (M3)
6. Bench rig and measured energy (M4)

In other words: channels, bus, LLM access, tool execution, persistence, and autonomous scheduling all exist; the new code is exactly two things — the spec layer and the experiment infrastructure.

## New modules

```
main/spec/
  spec_schema.h        # C struct definitions for script/guard/state
  spec_parser.c        # cJSON → structs, schema validation (reject malformed scripts)
  guard_eval.c         # guard evaluator: gt/lt/in/eq/delta_lt/within_s + AND (target <150 lines)
  spec_executor.c      # guarded-commit state machine (runs inside the agent loop task, see below)
  state_snapshot.c     # compact state block generation (repredict payload)
  commit_log.c         # execution log → SPIFFS; recovery point after power loss/reset
main/tools/
  tool_env.c           # virtual-environment tool: exchanges sensor/actuator messages with the host simulator via the WS gateway
bench/                 # host side (Python, new directory in-repo)
  env_sim/             # environment simulator: sensor models, noise/fault injection, ground-truth judging
  tasks/               # YAML definitions of the 18 tasks (initial state, injection schedule, judge function, budgets)
  runner/              # experiment orchestration: drives the MCU per config matrix (serial + WS), collects logs, asserts completion
  analysis/            # metric computation + plotting scripts for the six target figures
```

**Execution model**: `spec_executor` does not get its own FreeRTOS task; it is a mode branch inside `agent_loop` — at the top of the loop, check "is there an unexhausted script?"; if yes, take the local guarded-commit fast path; if no (or on mispredict), take the existing cloud-call path. This avoids two tasks contending for the tool registry and the message bus.

**Metering instrumentation** (in place from Phase 0, shared by all methods): `llm_proxy` records DNS/TLS-handshake/TTFB/total duration and bytes up/down per call; `esp_timer` step-level timestamps; `heap_caps` high-water sampling. On the bench, the INA226 is sampled over I2C by the firmware itself, and a spare GPIO toggle time-aligns the energy trace with step events.

## Milestones

| | Content | Deliverable / exit criterion | Estimate |
|---|---|---|---|
| **M0** | Metering instrumentation + env_sim skeleton + runner driving B1 | 3 tasks run fully automated under B1, 10 repetitions, metrics CSV out | 1.5 wk |
| **M1** | Spec protocol + parser + guard_eval + executor fast path; **smoke experiment** | First L and call-count comparison, Ours vs. B1, on the thermostat task; → post arXiv tech report | 2 wk |
| **M2** | state_snapshot + commit_log + D_irrev + malformed-script rejection | Mispredict→repredict loop stable; recovery from commit log after reset | 1.5 wk |
| **M3** | Full 18-task suite + noise/fault/outage injection + B1+/B2/B3 | Config matrix (5 methods × 3 stochasticity × 18 tasks × 10 seeds) runs fully automated | 2.5 wk |
| **M4** | Bench build + measured energy + ablations | Figures 1–6 all produced | 2 wk |
| **M5** | Paper writing + gap-filling experiments | Submission-ready draft | 3 wk |

**The M1 smoke experiment is the biggest risk probe**: if the thermostat task cannot reach L ≥ 3, or half the LLM-generated guards are malformed, we learn it before building out the full infrastructure — and pivot to protocol redesign (narrower guard language, two-shot prompting) instead of laying more surface.

## Isolation from mainline MimiClaw

All new code lives in `main/spec/` and `bench/`, behind a `CONFIG_MIMI_SPEC_MODE` menuconfig switch, default off — the open-source mainline (Telegram assistant use case) is unaffected, and the paper artifact is simply a build flag of the main repository.
