# Speculate Locally, Reason Remotely: Guarded Speculative Execution for LLM Agent Loops on Microcontrollers

**Research Proposal** — Draft v1.0, July 2026
Platform: MimiClaw (ESP32-S3 agent firmware)

---

## Abstract

Large language model (LLM) agents built on the ReAct paradigm interleave reasoning and action by issuing one model call per step. When the agent loop is hosted on a microcontroller (MCU) — a deployment model that has recently become practical, as demonstrated by MimiClaw, ESP-Claw, and related open-source systems — this call-per-step structure is uniquely expensive: every call pays a fixed cost in radio wake-up, TLS handshake, and round-trip latency that dominates energy consumption on battery-powered devices, independent of token count. We propose *guarded speculative execution*, an execution model in which a single cloud call drafts the next k steps of the plan as (action, guard envelope, timeout) tuples, and the MCU verifies and commits these steps locally using a deterministic guard evaluator of roughly one hundred lines of C, calling back to the cloud only when a prediction diverges from observed reality. Unlike the recent wave of server-side speculative agent systems, which optimize latency by eagerly executing discardable actions and verifying them with a second model, our setting inverts every core assumption: the verifier is the physical world interpreted by deterministic code rather than an LLM; the optimization target is the number of calls rather than latency overlap; and actions on physical actuators are irreversible, so speculation must follow commit-after-verify semantics and never eager execution. This inversion surfaces a new failure mode — *silent divergence*, in which a guard passes while the environment has semantically diverged — which we define, measure, and control through guard design and an irreversibility-aware depth bound. We will evaluate the system on real ESP32-S3 hardware across an 18-task suite spanning task horizon, environment stochasticity, and action irreversibility, reporting measured energy, uplink bytes, call counts, and latency alongside task success. The expected results include the first systems characterization of speculation depth as a function of environment predictability, and the first quantified account of disconnection resilience for device-hosted agent loops.

---

## 1. Introduction

The ReAct paradigm [1] structures an LLM agent as a loop: the model reasons about the current situation, selects an action, observes the result, and repeats. Nearly all research on such agents assumes the loop runs on a server or workstation. That assumption quietly broke in 2026: a cluster of open-source projects — Espressif's ESP-Claw, WireClaw, zclaw, and our own MimiClaw — demonstrated complete agent loops running on ESP32-class microcontrollers, with tool execution, memory, and channel management on the device and only the reasoning step delegated to a cloud LLM API. These systems work, but none of them has been studied. No published work characterizes their cost structure, and no published mechanism addresses their central inefficiency.

That inefficiency is structural. A ReAct loop issues one cloud call per step, and on an MCU each call carries a fixed overhead that has nothing to do with tokens: waking the radio, performing a TLS handshake (one to three seconds on an ESP32-S3 without connection reuse), and waiting out the round trip. For a twenty-step task the device pays this toll twenty times. Server-side agent research measures cost in tokens and has therefore never confronted this term; on a battery-powered device it is the dominant term.

We propose to attack the call count directly. In our design, which we call **SpecClaw**, the cloud model responds to each request not with a single action but with a short *speculation script*: the next k actions it expects the task to require, each annotated with a machine-checkable *guard envelope* describing the sensor conditions under which the step remains valid, and a timeout. The MCU executes this script step by step, evaluating guards with deterministic C code — no on-device model of any kind — and committing each action only after its preconditions pass. When a guard fails, the device halts speculation, snapshots a compact task state, and makes one cloud call to obtain a fresh script. If the environment behaves as predicted, a task that previously required n calls completes in n/L, where L is the mean number of steps accepted per script.

The idea transplants the logic of speculative execution — branch prediction in processors, speculative decoding in LLM inference [18] — to the agent loop, and a recent line of work has already done so on the server side [2–8]. Our contribution is not the analogy but its inversion under embedded constraints, which changes the problem in three ways that we believe make it a distinct and unstudied object (Section 2.2), the most consequential being irreversibility: a speculatively mis-executed search query costs nothing, but a speculatively mis-executed relay actuation cannot be taken back.

## 2. Background and Motivation

### 2.1 Platform: MCU-hosted agent loops

Our experimental platform, MimiClaw, is an open-source agent firmware for the ESP32-S3 (dual-core Xtensa LX7, PSRAM, 16 MB flash) built on ESP-IDF and FreeRTOS. The agent loop runs as a dedicated task: it pops user messages from a queue fed by Telegram, Feishu, and WebSocket channels, assembles a context from persona, memory, and session files stored on SPIFFS, calls the Anthropic or an OpenAI-compatible API over TLS, and executes returned tool calls (GPIO actuation behind a policy allowlist, file access, scheduled jobs, web search) for up to ten iterations per message. Cron and heartbeat subsystems already give the device an unattended, long-running operating mode. The system is, in effect, a complete ReAct agent in roughly the footprint of a consumer IoT firmware — and it serves both as evidence that the deployment model is real and as the baseline implementation for this proposal.

What neither MimiClaw nor its peer projects possess is any mechanism between "call the cloud every step" and "do not use the cloud at all." This proposal supplies that middle ground.

### 2.2 Why server-side speculation does not transfer

Speculative execution for LLM agents is an active area: draft-and-verify planning with two models [2], reasoning/action overlap in search agents [3], asynchronous background tool execution [4], pattern-guided speculation [5, 6], idle-time speculation [7], and the privacy consequences of speculative tool calls [8]. All of this work shares three assumptions, each of which fails in our setting.

First, *the verifier is a model*. Server-side systems check a draft agent's proposals against a stronger target model. An MCU cannot host any model; our verifier is the physical world itself, read through sensors and interpreted by a guard language expressly designed to be evaluated by trivial deterministic code.

Second, *the objective is latency*. Server-side speculation hides inference time by overlapping it with tool execution; calls remain plentiful but their serialization improves. Our objective is the number of calls, because on the device each call is a fixed quantum of radio energy and handshake latency. The same mechanism, aimed at a different cost term, yields different design choices — depth matters more than overlap, and there is no benefit to issuing speculative calls in parallel.

Third, *actions are discardable*. Prefetching a search result that turns out to be unneeded wastes only tokens. Actuating a motor, releasing irrigation water, or latching a relay cannot be discarded. Speculation over physical actions must therefore be *commit-after-verify*: no action executes until its guard passes, and irreversible actions are further bounded by a depth limit D_irrev — the number of irreversible commits permitted since the last cloud confirmation. This constraint has no analogue in the server-side literature and generates the central new research object of this proposal: **silent divergence**, the event in which a step's guard passes but the environment has already departed from the task's semantic intent, causing a wrong physical action that no subsequent replanning can undo.

### 2.3 A secondary motivation: disconnection resilience

Because the speculation script resides on the device, the MCU can continue executing its verified prefix during a network outage. A cloud-orchestrated agent stalls the moment connectivity drops. Speculation therefore doubles as an availability mechanism, and we treat outage survival as a first-class evaluation axis rather than an afterthought.

## 3. Related Work

**Server-side speculative agents.** The systems cited above [2–8] establish the vocabulary of drafting, verification, and acceptance for agent loops but operate entirely within the three assumptions of Section 2.2. We position our work as the embedded counterpoint: world-as-verifier, calls-as-cost, irreversible actions.

**Plan-then-execute agents.** ReWOO [9] and the LLM Compiler [10] generate a complete plan in one call and execute it without per-step feedback, capturing the call-count benefit but none of the safety: an open-loop plan executes blindly through environmental drift. ReWOO-style execution is our baseline B2, and we expect it to fail precisely where guards earn their keep.

**Device–cloud closed loops.** EcoAgent [11] is the closest precedent: a cloud planner emits a full step list with per-step expectations, and on-device agents execute and verify each step, escalating to the cloud on failure. The critical difference is that EcoAgent's device-side verification is performed by a 2B-parameter vision-language model on a smartphone. No model of any size runs on an MCU. Our guard language exists to replace that model with deterministic code — which also makes verification auditable, a property a VLM judge cannot offer. Beyond this, EcoAgent's speculation depth is fixed at "the whole plan," it does not distinguish reversible from irreversible actions, and its evaluation reports calls and tokens but not energy or bytes.

**LLM-to-MCU protocols.** The Device Context Protocol [12] takes the opposite architectural stance — the loop stays on a host and the MCU executes pre-validated commands — and contributes capability scoping and range checking that we adapt into our guard language. The comparison between loop-on-device and loop-on-host under disconnection is part of our evaluation.

**Context and state compression.** MEM1 [13], StateAct [14], and recent constant-context formulations [15] compress agent history into rewritten or structured state. We borrow from this line for our repredict payload (a fixed-size plain-text state block; StateAct's finding that JSON-formatted state degrades accuracy directly informs its serialization) but note that none of this work addresses execution semantics, which is our core subject. Observation masking [16] is the strong cheap baseline from this literature and appears in our ablations.

**Pre-LLM embedded agents.** BDI architectures compiled to bare-metal firmware [17] show that agent loops on MCUs have a pre-LLM lineage; what is new in the LLM era is that the policy is a remote, expensive, fallible oracle — exactly the conditions under which speculation pays.

## 4. Proposed Approach

### 4.1 The speculation script

Each cloud call returns, via structured output, a script of the form:

```json
{
  "state_update": "relay=on; last_temp=27.3; goal_phase=cooling",
  "script": [
    { "step": 1,
      "action": {"tool": "gpio", "args": {"pin": 5, "level": 1}},
      "irreversible": false,
      "guard": {
        "pre":  [{"var": "temp_c", "op": "gt", "val": 26.0}],
        "post": [{"var": "temp_c", "op": "lt", "val": 26.5, "within_s": 120}]
      },
      "on_fail": "replan" }
  ],
  "done_when": [{"var": "temp_c", "op": "lt", "val": 25.0}]
}
```

The guard language is deliberately minimal: numeric comparisons, discrete equality, bounded rate-of-change, time windows, and conjunction only. Restricted expressiveness is the point — guards must be decidable, evaluable in about a hundred lines of C, and statically auditable. Whether an LLM can *reliably generate* sufficient guards in so small a language is itself one of our research questions (RQ3).

### 4.2 Guarded-commit semantics

Execution proceeds step by step: evaluate the pre-guard (mispredict if it fails), enforce the irreversibility depth bound D_irrev (forcing a cloud confirmation if exceeded), execute the action through the firmware's existing policy allowlist, await the post-guard within its time window (mispredict on violation), and append the step to a persistent commit log. On mispredict, the device generates a compact state snapshot — goal, current sensor state, committed prefix, the failed guard with observed values, and a bounded list of salient facts, serialized as plain structured text of 300–500 bytes — and makes one cloud call to obtain a fresh script. The uplink payload is thus O(1) per replan, in contrast to the O(n)-growing transcript of standard ReAct.

Two invariants define the model: no action executes before its pre-guard passes, and the speculation layer never bypasses the safety layer beneath it.

### 4.3 Definitions

We define **acceptance length L** as the number of consecutive steps verified and committed per cloud call (the systems analogue of acceptance length in speculative decoding); **silent divergence** as a committed step whose guard passed under a semantically diverged environment (guard false-accept, judged against simulator ground truth); and **guard false-reject** as a replan triggered by an over-tight guard in a non-diverged environment. These three quantities, together with D_irrev, parameterize the entire design space.

## 5. Research Questions

**RQ1 (benefit).** At matched task success, how much does guarded speculation reduce cloud calls, energy, and median step latency relative to step-by-step ReAct? *Hypothesis: calls fall by a factor of L ∈ [3, 8]; energy per task falls by at least 60% because fixed per-call overhead dominates; the step-latency distribution becomes bimodal (millisecond local steps, second-scale cloud steps).*

**RQ2 (depth versus stochasticity).** How does the profitable speculation depth vary with environment predictability? *Hypothesis: in deterministic environments L grows with offered depth k; under increasing noise and injected faults L collapses toward 1, and a measurable crossover exists beyond which speculation yields no net benefit. This curve — "how deep is worth speculating, in which environments" — is the core empirical contribution.*

**RQ3 (guard quality).** What false-accept/false-reject trade-off do LLM-generated guards exhibit, and how do envelope tightness and guard density move it? *Hypothesis: default generations are too loose (nonzero silent divergence); prompting for at least one observable post-condition per action suppresses silent divergence at modest false-reject cost, tracing an ROC-shaped frontier.*

**RQ4 (disconnection).** Under injected outages of 10, 60, and 600 seconds, how much higher is task survival for speculative execution than for step-by-step ReAct and a cloud-orchestrated arrangement? *Hypothesis: tasks whose outage falls within the verified prefix complete unaffected, and the survival gap widens with depth.*

## 6. Evaluation Plan

**Substrate.** All methods, including baselines, are build configurations of the same MimiClaw firmware, eliminating implementation confounds. The primary substrate is hardware-in-the-loop with a simulated environment: the ESP32-S3 runs real firmware and makes real API calls, while sensors and actuators are served by a host-side Python simulator attached through the firmware's existing WebSocket gateway. Simulation is required because silent divergence is only measurable against ground truth, and because stochasticity must be injected reproducibly. A physical bench (temperature sensor, relay-driven fan, INA226 current monitor) runs a task subset to ground the energy numbers in measurement rather than simulation.

**Task suite.** Eighteen tasks arranged on a grid of three dimensions: horizon (3–5, 8–12, 20+ steps), environment predictability (deterministic; Gaussian sensor noise with outliers; adversarial injection of I2C errors, actuator failures, and state jumps), and irreversibility (read-only; reversible actuation; irreversible actuation such as metered water release). Each task ships a ground-truth judge on the simulator side. Representative tasks: thermostat regulation, timed irrigation, multi-sensor fault diagnosis, ordered actuator power-up sequences, and constraint-carrying scheduling tasks that test fact retention across replans.

**Baselines.**

| ID | Description |
|---|---|
| B1 | Step-by-step ReAct (the existing MimiClaw loop) |
| B1+ | B1 with Anthropic prompt caching, preempting the objection that caching already solves cost |
| B2 | ReWOO-style plan-then-execute: one full plan, open-loop, no guards |
| B3 | EcoAgent's design adapted to MCU feasibility: full-length plan with naive equality guards, whole-plan replan on any failure |
| Ours | Tunable depth k, envelope guards, compact-state repredict, D_irrev |

**Metrics.** Task success against ground truth; mean acceptance length L; cloud calls, input/output tokens, and uplink bytes per task; joules per task (bench subset); the per-step latency distribution; silent-divergence and false-reject rates; PSRAM high-water mark; and outage survival rate. At least ten seeds per configuration with 95% confidence intervals.

**Ablations.** Guard density (none / one / several post-conditions per step); envelope tightness (model default versus prompt-tightened); repredict payload (compact state versus full transcript, connecting to the context-compression literature); D_irrev ∈ {0, 1, 3, ∞}; and model tier (Haiku versus Sonnet), asking how much model capability the protocol demands. One adaptive variant — the model chooses its own speculation horizon and truncates the script at uncertain points — is compared against fixed depths; learned schedulers are out of scope.

## 7. Expected Contributions

1. A speculative-execution semantics for agent loops with irreversible physical actions — guarded commit with an irreversibility depth bound — together with the definition and measurement methodology for silent divergence, a failure mode absent from server-side speculation.
2. A minimal guard language that a cloud LLM can generate and an MCU can evaluate deterministically, replacing the device-side model verification of prior device–cloud closed loops.
3. The first systems evaluation of an MCU-hosted agent loop on real hardware, in physical units — joules, bytes, calls — including the depth-versus-stochasticity characterization and quantified disconnection resilience.
4. An open-source artifact: firmware (a build flag of the MimiClaw mainline), the environment simulator, and the task suite, intended as groundwork for a future MCU-agent benchmark.

## 8. Work Plan

Six milestones over approximately thirteen weeks. **M0** (1.5 wk): per-call metering instrumentation (handshake, TTFB, bytes), the simulator skeleton, and an automated runner exercising baseline B1. **M1** (2 wk): the script protocol, parser, guard evaluator, and executor fast path, culminating in a smoke experiment on the thermostat task; an arXiv technical report is posted at this point to stake the claim. **M2** (1.5 wk): state snapshotting, the persistent commit log, D_irrev enforcement, and malformed-script rejection. **M3** (2.5 wk): the full task suite, stochasticity and outage injection, and the remaining baselines, ending with the complete configuration matrix running unattended. **M4** (2 wk): bench construction, measured energy, and ablations, producing all target figures. **M5** (3 wk): paper writing and gap-filling experiments.

The M1 smoke experiment is deliberately scheduled as the earliest possible risk probe: if the thermostat task cannot achieve L ≥ 3, or a large fraction of generated guards is malformed, the protocol is redesigned before the experimental surface is built out.

## 9. Risks

The principal empirical risk is a short measured L, which would deflate the headline benefit; the mitigation is that the fixed-overhead accounting already pays at L = 2, and that RQ2 frames "when speculation is not worth it" as a publishable finding rather than a failure. The principal reviewing risks are the comparison to EcoAgent (answered by five concrete deltas: deterministic guards in place of a device-side VLM, irreversibility semantics, tunable depth, physical-unit metrics, and disconnection analysis) and skepticism toward simulated environments (answered by the physical bench and by simulator noise parameters drawn from real sensor datasheets). The field-level risk is scooping — this area has moved in six-month waves — which the M1 arXiv posting addresses; the specific intersection of speculation, MCUs, and irreversible actuation is unoccupied at the time of writing.

## 10. Dissemination

Primary venues: SenSys, MobiSys, or IPSN for the full paper; a four-page version to a HotMobile/HotEdge-class workshop as an early stake. The artifact ships as a build configuration of the public MimiClaw repository.

---

## References

[1] Yao et al. *ReAct: Synergizing Reasoning and Acting in Language Models.* arXiv:2210.03629.
[2] Guan et al. *Dynamic Speculative Agent Planning.* arXiv:2509.01920.
[3] *SPAgent: Reducing Latency of LLM Search Agents via Speculation-Based Algorithm–System Co-Design.* arXiv:2511.20048.
[4] *Sherlock: Reliable and Efficient Agentic Workflow Execution.* arXiv:2511.00330.
[5] *Act While Thinking: Accelerating LLM Agents via Pattern-Aware Speculative Tool Execution.* arXiv:2603.18897.
[6] *B-PASTE: Beam-Aware Pattern-Guided Speculative Execution for Resource-Constrained LLM Agents.* arXiv:2604.16469.
[7] *IdleSpec: Exploiting Idle Time via Speculative Planning for LLM Agents.* arXiv:2605.22154.
[8] *Ghost Tool Calls: Issue-Time Privacy for Speculative Agent Tools.* arXiv:2606.02483.
[9] Xu et al. *ReWOO: Decoupling Reasoning from Observations for Efficient Augmented Language Models.* arXiv:2305.18323.
[10] Kim et al. *An LLM Compiler for Parallel Function Calling.* arXiv:2312.04511.
[11] Yi et al. *EcoAgent: An Efficient Device–Cloud Collaborative Multi-Agent Framework for Mobile Automation.* AAAI 2026. arXiv:2505.05440.
[12] Yang. *Device Context Protocol: Safety-First LLM Control of Constrained Devices.* arXiv:2605.26159.
[13] *MEM1: Learning to Synergize Memory and Reasoning for Efficient Long-Horizon Agents.* arXiv:2506.15841.
[14] Rozanov and Rei. *StateAct: Enhancing LLM Base Agents via Self-Prompting and State-Tracking.* arXiv:2410.02810.
[15] *Remember, Don't Re-read: Stateful ReAct Agents for Token-Efficient Autonomous Experimentation.* arXiv:2606.14945.
[16] Lindenbauer et al. *The Complexity Trap: Simple Observation Masking Is as Efficient as LLM Summarization for Agent Context Management.* arXiv:2508.21433.
[17] *Embedding Autonomous Agents in Resource-Constrained Robotic Platforms.* arXiv:2601.04191.
[18] Leviathan et al. *Fast Inference from Transformers via Speculative Decoding.* arXiv:2211.17192.
