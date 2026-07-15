# MimiClaw: Running a Complete LLM Agent Loop on a $4 Microcontroller

**Technical Report** — v1.0, July 2026
~8,300 lines of C (main/), ESP-IDF + FreeRTOS, ESP32-S3
Code: https://github.com/memovai/mimiclaw

---

## Abstract

LLM agent runtimes have been shrinking steadily: from OpenClaw's 400,000+ lines of TypeScript, to nanobot's 4,000 lines of Python, to sub-10 MB Go and Rust implementations such as PicoClaw and ZeroClaw. This trajectory stops, collectively, at one boundary: all of these systems require Linux — processes, a hierarchical filesystem, a dynamic language runtime, a package manager. MimiClaw crosses that boundary. It implements a complete agent runtime in roughly 8,300 lines of C on a bare-metal ESP32-S3 with no operating system, no processes, and 512 KB of on-chip SRAM: a ReAct tool loop, persistent memory managed by the model itself, an extensible skill system, multi-channel messaging, scheduled and heartbeat-driven autonomous behavior, and deterministic safety guards for hardware actuation, with reasoning delegated to a cloud LLM API. This report argues for the central claim this system demonstrates: **the essence of an agent runtime does not depend on an operating system.** We show how each Linux-era agent primitive is rebuilt on embedded primitives — processes become FreeRTOS tasks, async frameworks become message queues, plugin systems become Markdown data files, vector memory stores become plain-text files the model reads and writes itself — and we provide a complete resource ledger for this reconstruction, four engineering principles that recur throughout the design, and an honest account of the open problems it exposes.

---

## 1. Introduction

What does an LLM agent runtime actually require? Stripped of frameworks, the answer is five things: a loop that cycles through reasoning, tool execution, and observation; a set of tools and an environment to run them in; memory that persists across sessions; message channels to the outside world; and autonomous behavior when no one is talking to it. The evolution of agent runtimes since 2025 has been a sequence of demonstrations that some dependency previously assumed essential was in fact accidental: nanobot showed the 400,000-line framework was unnecessary; PicoClaw and ZeroClaw showed Python was unnecessary. All of them, however, stopped at Linux — leaving one assumption untested: **does an agent runtime need an operating system?**

MimiClaw answers no, and answers constructively: not with an argument but with a system that has been running on real hardware for an extended period. The device holds all state, orchestration logic, and safety boundaries; the cloud model is a stateless reasoning oracle that knows nothing about the device between calls. This division — **intelligence in the cloud, architecture on the device** — sharpens the question considerably: if intelligence consumes no local resources, how large is the minimal sufficient implementation of the *architectural* part of an agent, and what does it take to build one without an OS? The rest of this report is a measured answer.

The result is not a toy. The device connects directly to Telegram and Feishu, stays online continuously, updates over the air, and returns from power loss with its memory intact. Peer systems that appeared in 2026 — Espressif's official ESP-Claw, WireClaw, zclaw — corroborate that MCU-hosted agent loops now constitute a real class of systems. MimiClaw is among the earliest implementations of that class.

## 2. Five Constraints of the Bare-Metal Environment

Moving an agent loop from Linux to bare metal means crossing five distinct gaps.

**No processes or scheduler abstraction.** Runtimes like nanobot rely on an async framework (asyncio) for concurrency: polling channels, running tools, and calling APIs without blocking one another. Bare metal offers only static FreeRTOS tasks and queues, and each task's stack is fixed at creation — too small and it overflows; too large and it wastes scarce on-chip SRAM.

**Scarce, tiered memory.** The ESP32-S3 has 512 KB of on-chip SRAM, a substantial fraction of which is consumed by the WiFi stack and TLS sessions. External PSRAM adds 8 MB, but with slower access and DMA restrictions. A single LLM call — system prompt, 20-message history, tool schemas, plus a response buffer — requires tens of kilobytes. Placed wrongly, they exhaust the memory the system needs to function.

**No real filesystem.** SPIFFS is flat: no directories, only files with path-like names, and bounded name lengths (this project had to raise `SPIFFS_OBJ_NAME_LEN` to 64 to stop long session filenames from truncating). Yet the established agent memory paradigm — OpenClaw's MEMORY.md, daily notes, skill directories — is built around a directory tree.

**No dynamic language runtime.** On Linux, agents extend their capabilities by hot-loading Python or JavaScript plugins. On bare metal, code is firmware: changing a line means recompiling and reflashing. If extending the agent required that path, the system would be dead on arrival.

**Failure is the normal case.** Power loss, WiFi outages, watchdog resets, and NVS corruption happen routinely, and there is no operator on site. A resident agent must assume it can be hard-reset at any moment and must recover without human intervention.

## 3. System Design

### 3.1 Architecture: a Message Bus on a Dual-Core Split

The skeleton of the system is a message bus built from two FreeRTOS queues (inbound and outbound, depth 16 each). Every channel — Telegram long polling, Feishu webhook/polling, the WebSocket gateway, cron, heartbeat — does exactly one thing: push messages onto the inbound queue. The agent loop pops from inbound, runs the ReAct cycle, and pushes results onto outbound; a separate dispatch task routes outbound messages back to the correct channel. Channels and agent are fully decoupled: adding a channel touches no agent code, and channels keep receiving while the agent blocks on an LLM call.

The bus is laid over a physical dual-core split: **the agent loop owns core 1 (priority 6); all I/O — three channel tasks, outbound dispatch, the serial CLI — shares core 0.** Reasoning and I/O therefore cannot preempt each other at the hardware level: TLS decryption for Telegram polling never interrupts tool execution, and the agent's JSON assembly never delays message receipt. This is the complete translation of asyncio's concurrency semantics into embedded primitives: the async framework becomes queues plus a static dual-core layout; callbacks become blocking tasks, each minding its own station.

Message contents are passed by pointer with explicit ownership transfer (pushing transfers ownership; the failing party frees). When a queue is full, the message is dropped and logged rather than blocking — the backpressure policy is to lose a status message rather than stall the agent.

### 3.2 Memory: a Tiering Policy and Degraded Startup

Memory management reduces to one discipline: **large and cold goes to PSRAM; small and hot stays in SRAM.** The agent's three working buffers — a 16 KB system prompt, a 32 KB history/stream buffer, an 8 KB tool output buffer — are allocated from PSRAM once at task startup and reused for the lifetime of the system (no per-iteration malloc, hence no fragmentation). Task stacks, TLS sessions, and WiFi buffers stay in on-chip SRAM. All buffers are fixed-size and all writes are bounds-checked, so the system's memory footprint has a compile-time upper bound.

One detail captures the bare-metal survival philosophy: **degraded startup.** When creating the agent task, the firmware tries a descending ladder of stack sizes — 24, 20, 16, 14, 12 KB — and uses the first that succeeds, logging remaining heap at each failure. In an environment where available memory drifts with firmware version, connection count, and TLS state, this yields a wide operating band where the alternative would be a binary choice between full configuration and a crash. Graceful degradation is preferred to precise budgeting.

### 3.3 Memory and Context: the File Paradigm on Flat Flash

MimiClaw's memory system has no vector database and no embedding model. It is a faithful port of the OpenClaw file paradigm: `SOUL.md` (persona), `USER.md` (user profile), `MEMORY.md` (long-term memory), `daily/<date>.md` (daily notes), and `sessions/` (conversation history), all plain text on SPIFFS. The decisive design choice: **the writer of memory is the model itself.** The firmware provides only file read/write tools and a memory discipline in the system prompt ("write new facts about the user to MEMORY.md immediately"; "read before writing; edit rather than overwrite"; "summarize, don't dump"). What to record, when, and how to organize it are decided by the LLM during conversation. The device thereby acquires a personality that survives power loss and resets — implemented in roughly a hundred lines of firmware.

Context assembly is a fixed-budget scheme: the system prompt is capped at 16 KB, with sub-caps of 4 KB for long-term memory, 4 KB for the last three days of notes, and 2 KB for the skill summary; session history is capped at 20 messages. Two companion mechanisms deserve mention:

- **Session slimming.** Persisted sessions keep only the user's text and the final assistant reply; the intermediate ReAct traffic (tool calls and results) is discarded after use. The large volume of intermediate tokens produced by multi-step tool use never accumulates into the next turn's context — a first gate, at the protocol level, against context growth with task length.
- **Progressive disclosure of skills.** Skills are Markdown files on SPIFFS; the system prompt carries only a 2 KB summary listing them. When the model judges that a task matches a skill, it loads the full file itself via `read_file`. Extending the agent's capabilities thus becomes *writing a text file* — no recompilation, no reflashing; the agent can even write new skills for itself during a conversation. This also answers the dynamic-runtime constraint: encode behavior as data, and let the interpreter be the cloud LLM.

### 3.4 Tools and Deterministic Guards: the Firmware Does Not Trust the Model

The tool layer is designed on the premise that the model will make mistakes, and that correction must not require asking the model again. Two mechanisms embody this.

**A policy allowlist** (`gpio_policy`): every GPIO operation is validated against a static allowlist in firmware before execution. Whatever parameters the model generates, pins outside the allowlist are unreachable. The safety boundary is defined by C code; the prompt is merely a courtesy.

**Parameter patching.** In practice the model calling `cron_add` would omit or mis-fill the delivery channel and chat ID (e.g., using the literal string "cron" as a Telegram chat ID). The firmware's response is not retry but **deterministic repair of tool inputs**: fields whose correct values the firmware knows from the current message context are overwritten before execution. We consider this a generalizable pattern: *between model output and side effect, any field whose correct value the firmware knows with certainty is decided by the firmware.*

### 3.5 Autonomy and Fault Recovery

A cron service (`cron.json` on SPIFFS, up to 16 jobs, 60-second granularity) and a heartbeat (reading `HEARTBEAT.md` every 30 minutes to wake an agent turn) give the device behavior in the absence of incoming messages: scheduled reports, self-checks, and follow-ups the model schedules for itself — cron jobs are themselves created by the model through a tool, so the agent can arrange its own future behavior.

Fault recovery runs through every layer: corrupted NVS is erased and rebuilt; a failed SPIFFS mount triggers reformatting; repeated WiFi failure drops the device into a captive-portal provisioning hotspot; a full outbound queue drops and logs rather than blocking. All durable state — memory, sessions, cron, configuration — lives in flash, so after a watchdog reset or power loss the device returns with its memory intact. Together with dual OTA partitions and a serial CLI, this is a device that can be shipped to a non-technical user and picks itself up when it falls.

## 4. Resource Ledger

**Flash (16 MB):** 2 × 2 MB OTA application partitions + 12 MB SPIFFS (memory/sessions/skills/config). The firmware itself is about 2 MB — a complete agent runtime in the code footprint of a mid-sized npm dependency.

**RAM:** static task stacks total about 56 KB of on-chip SRAM (agent 24 KB; three channel/dispatch tasks at 12 KB each; CLI 4 KB); the agent's 56 KB of working buffers (16 + 32 + 8 KB) reside entirely in PSRAM. Roughly stated: **the architectural part of an agent runs in under 60 KB of on-chip memory**, leaving the rest to the network stack and TLS.

**Context budget:** system prompt ≤ 16 KB (with the itemized sub-caps above), history ≤ 20 messages, single tool output ≤ 8 KB.

**Code:** about 8,300 lines of C in main/, 214 commits, multiple contributors. For reference, nanobot is about 4,000 lines of Python — but no Python interpreter fits on an MCU. The 8,300 lines are the complete figure, including all channels, memory, tools, provisioning, and OTA, with no dependencies hidden inside a runtime.

## 5. Position Among Peer Systems

| System | Language / base | Minimum hardware | Loop location | Notes |
|---|---|---|---|---|
| OpenClaw | TypeScript / Node | PC-class | local process | most featureful; 400k+ lines |
| nanobot | Python / Linux | Raspberry Pi-class | local process | 4k lines; the template for file-based memory |
| PicoClaw / ZeroClaw | Go / Rust, Linux | $10 SBC | local process | <10 MB; still requires an OS |
| ESP-Claw (Espressif) | C / ESP-IDF | ESP32 | **MCU** | vendor-backed system of the same class |
| zclaw | C / ESP-IDF | ESP32 | **MCU** | ≤888 KB full firmware; the size extreme |
| **MimiClaw** | C / ESP-IDF | ESP32-S3 | **MCU** | full memory/skill/autonomy stack; dual-core bus architecture |

Within the MCU group, MimiClaw's differentiation is **architectural completeness**: the dual-core message bus, model-managed tiered memory, progressive-disclosure skills, deterministic guards with parameter patching, and cron/heartbeat autonomy. What it ports is not "a firmware that can chat" but the full runtime semantics of a nanobot-class agent.

## 6. Evaluation Plan

There is no established methodology for evaluating an MCU-hosted agent runtime. Existing agent benchmarks — AgentBench, ToolBench, WebArena, AndroidWorld — all assume the loop runs on a host and the tools are web or GUI operations; none of them can execute on, or meaningfully stress, a device-resident runtime. We therefore evaluate along three axes — systems cost, task capability, and reliability — each with its own metrics, workloads, and comparison targets. The absence of a suitable benchmark is itself part of the finding: the task suite below is designed to be released as the seed of an MCU-agent benchmark.

### 6.1 Metrics

| Category | Metrics |
|---|---|
| Systems footprint | flash size; SRAM and PSRAM high-water marks (`heap_caps`); boot-to-ready time; idle power |
| Per-turn cost | end-to-end latency with decomposition (DNS / TLS handshake / TTFB / token streaming / local parse / tool execution); uplink and downlink bytes; input/output tokens; dollars per task; joules per turn (INA226/PPK2) |
| Task capability | success rate against programmatic ground truth; tool iterations per task; redundant-call rate |
| Memory quality | cross-session recall accuracy (probe facts planted k days earlier); recall after hard reset; memory-file growth rate |
| Reliability | 7-day soak test (crash count, heap drift, reconnect count, message loss); recovery rate under injected faults (WiFi outage, watchdog reset, SPIFFS corruption) |
| Safety | unsafe-action block rate at `gpio_policy`; parameter-patch hit rate (already logged by the firmware) |

The per-call instrumentation this requires (timing and byte counters in `llm_proxy`) is the same infrastructure specified as milestone M0 of the SpecClaw proposal; the two efforts share it.

### 6.2 Workloads

Two complementary sources. First, a **host-portable subset of existing function-calling tasks** (BFCL-style tool-selection and argument-filling problems that require no GUI), run once through MimiClaw on-device and once through a host-side reference loop with the same model: this isolates the runtime's overhead and any success-rate cost, with model capability held constant. Second, a **purpose-built MCU task suite** in five categories — sensing/actuation (GPIO), scheduling (cron), cross-session memory recall, multi-step orchestration, and fault recovery — each task with a programmatic ground-truth judge, executed against a host-side simulator attached through the WebSocket gateway plus a small physical testbed. The first source anchors comparability; the second measures what only a device runtime can do.

### 6.3 Baselines and comparison targets

"Baseline" here means five distinct things, each serving a distinct question, and the plan uses all five: (i) **peer runtimes** (nanobot, OpenClaw) for capability parity; (ii) a **minimal host-side reference loop** running the same tasks with the same model, to isolate the overhead of the MCU platform itself; (iii) the system's **own ablated variants** (one mechanism switched off at a time), to price each design claim; (iv) the **opposite architecture** (loop on a host, MCU as a bare tool endpoint), to test the loop-placement claim; and (v) **published numbers** on standard benchmarks (e.g., MEM1 and StateAct on ALFWorld) as an external anchor. The table below lists the external targets; §6.4 covers type (iii).

| Baseline | Same tasks, same model | Question it answers |
|---|---|---|
| nanobot on a Raspberry Pi (Zero 2 W and Pi 5) | yes | What does dropping the OS cost and buy? Expected: parity in success, MimiClaw ahead on unit cost, idle power, and boot time; behind on flexibility |
| Peer MCU systems: ESP-Claw, zclaw, WireClaw | common subset (chat, GPIO, scheduled task) | Where does MimiClaw sit within its own class? None has published measurements, so this comparison is itself a contribution |
| Cloud-orchestrated architecture (loop on host, MCU as a DCP/MCP-style tool endpoint) | yes | Is hosting the loop on the device worth it? Key differentiators: disconnection survival, local state, bytes on the wire |
| Model-tier sweep (Haiku / Sonnet / Opus) on the fixed runtime | yes | How much model capability does the runtime demand — and does the file-paradigm memory discipline degrade gracefully with weaker models? |

### 6.4 Ablations

Each of the report's distinctive mechanisms maps to a switch, turning design claims into measured deltas: session slimming vs. persisting full transcripts (context growth, cost per turn, success); progressive disclosure vs. injecting all skills into the prompt (prompt tokens, skill-task success); parameter patching on/off (scheduled-task success rate); the memory-discipline prompt on/off (recall accuracy, memory-file growth). Three refinements sharpen these: parameter patching is compared not only against "off" but against "off plus one model retry," to show deterministic repair beats paying an extra call; the dual-core split is compared against pinning everything to one core, measuring channel-latency jitter during agent turns — turning the architectural claim of §3.1 into data; and the context budget is swept (history of 5/10/20/40 messages; memory caps of 2/4/8 KB) to trace the success-vs-cost curve and locate its knee, testing whether the current defaults are near-optimal or merely habitual.

A further ablation family follows from a code-level finding: within a single turn, the loop re-uploads the entire growing message array on every tool iteration (the request body is rebuilt from a deep copy of the full array on each call), so within-turn uplink grows quadratically with iteration count. We therefore implement a **context-compaction ladder** — L0: full append (current behavior, the baseline); L1: per-tool output budgets with head/tail truncation; L2: deterministic iteration digests (iterations older than the most recent keep their tool_use/tool_result pairing, as the API requires, but results shrink to a one-line status and interim text blocks are dropped); L3: bare placeholder masking, the on-device reproduction of the observation-masking baseline [Complexity Trap, arXiv:2508.21433] — and measure bytes uploaded per turn, tokens, latency, and success at each level. The ladder doubles as the L1/L2 rungs of the TinyReAct research direction, so the same firmware switches serve both the engineering optimization and the research ablation.

### 6.5 Planned experiments

| # | Experiment | Baseline type (§6.3) | Expected artifact |
|---|---|---|---|
| A1 | Parity test on the common task set | i | parity table + capability coverage matrix |
| A2 | ALFWorld/WebShop subset run through the device (env host-side via the WS gateway) | ii, v | standard-benchmark table with joules/bytes columns no host system can report |
| A3 | Model-tier sweep (Haiku/Sonnet/Opus) on the fixed runtime | iii | harness-sensitivity curve; do deterministic guards compensate for weaker models? |
| B1 | Per-turn latency waterfall (DNS / TLS handshake / TTFB / streaming / parse / tool) | ii | waterfall chart; TLS connection-reuse on/off delta |
| B2 | Energy: joules per task, idle draw, 24-hour always-on total | i (nanobot on Pi Zero 2 W) | battery-life / annual-energy comparison |
| B3 | Context-budget sweep | iii | success-vs-cost curve with knee point |
| C1 | Fault-injection matrix (WiFi outages, mid-task power pull, API 429/500/malformed, full SPIFFS) | i, iii (recovery mechanisms disabled) | recovery rate and MTTR per fault class |
| C2 | 7-day soak | i | heap-drift curve; crash/reconnect/message-loss counts; memory-file growth |
| C3 | Dual-core split vs. single-core pinning | iii | channel-latency jitter during agent turns |
| D | Mechanism ablations (§6.4) | iii | per-mechanism deltas |
| D2 | Context-compaction ladder (L0 full append → L1 output budgets → L2 iteration digests → L3 placeholder masking) | iii | bytes-uploaded-per-turn curve vs. success; within-turn O(k²) → O(k) uplink demonstration |
| E1 | Memory recall probes (facts planted day 1; probed days 1/3/7 and across reboots) | iii (session-only lower bound; full-transcript upper bound) | recall curves; the file paradigm should land between the bounds, near the top |
| F1 | Cloud-orchestrated shim (loop on host, MCU as tool endpoint) | iv | disconnection survival, bytes on the wire, latency |

Sequencing follows cost-effectiveness. The first wave is every type-(iii) experiment (B1, B3, C3, D): they require no external system, invite no fairness disputes, and each directly supports a claim this report already makes — and if an ablation shows no difference, the corresponding claim is removed early. The second wave adds external hardware and systems (A1, B2, C1, E1). The third wave is the heavy investments (A2, which is required only if targeting an ML venue; C2, cheap to run but long in calendar time, so started early; F1, shared with the SpecClaw proposal, where it doubles as a baseline).

### 6.6 Protocol

At least 10 runs per configuration with 95% confidence intervals; fault injections replayed from fixed schedules; the harness, task definitions, and raw logs released with the code.

## 7. Four Design Principles

Looking back across the design, four principles recur; together they form a design grammar for the bare-metal agent runtime as a species.

1. **Behavior is data.** Anything that may change — persona, memory, skills, scheduled jobs — is encoded as text files in flash and interpreted by the cloud model; the firmware implements only invariant mechanisms. This sidesteps the absence of a dynamic runtime and gives the agent the ability to modify its own behavior.
2. **Every resource has a budget.** Every buffer, context layer, and queue has a hard bound visible at compile time. Bare metal has no swap and no OOM killer; an unknown upper bound is a guaranteed crash.
3. **The firmware does not trust the model.** Safety boundaries (allowlists) and known facts (parameter patching) are enforced by deterministic code; prompts guide, C code guarantees.
4. **Degradation over perfection.** The stack-size ladder at startup, the provisioning-portal fallback, drop-and-log on full queues, erase-and-rebuild on corruption — every subsystem has answered "and what if this step fails?", and the answer is never "it won't."

## 8. Limitations and Next Steps

Honest boundaries matter as much as claims. **First**, the device currently depends on the cloud at every step: ReAct issues one API call per step, each paying a fixed cost independent of tokens (radio wake-up, TLS handshake, round trip), and during an outage the loop survives but reasoning halts. This is the direct motivation for our follow-up work on guarded speculative execution (see `specclaw-research-proposal.md`), for which the bus, tool allowlist, and file persistence described here are precisely the prerequisites. **Second**, this report provides a design and a static ledger; the measurement plan in Section 6 is specified but not yet executed, and carrying it out is the work required to move from a systems report to a systems paper. **Third**, session slimming and fixed budgets control context volume, but growth of the memory files themselves still relies on the model's discipline; there is no firmware-enforced compaction.

These three limitations share a property: **each is a problem that only becomes visible once the system exists.** That is MimiClaw's value as a research platform — it turns "agents on MCUs" from a hypothesis into a set of concrete questions that can be measured, improved, and published.

---

*All mechanisms described in this report can be located in the source: message bus in `main/bus/`, agent loop in `main/agent/`, memory in `main/memory/`, guards in `main/tools/gpio_policy.c`, degraded startup in `main/agent/agent_loop.c` (`agent_loop_start`).*
