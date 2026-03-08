# AGENTS.md

## Purpose

This repository uses a markdown-first layered memory system. Chat context is
not the durable source of truth.

## Read Order

Before starting substantial work, read in this order:

1. `AGENTS.md`
2. `resume-queue.md`
3. `MEMORY.md`
4. The most relevant fact docs for the task
5. The latest relevant file under `memory/`
6. `knowledge/` only when detailed evidence is needed

## Fact Docs

Default fact docs in this repo:

- `README.md`
- `docs/ARCHITECTURE.md`
- `main/idf_component.yml`
- `sdkconfig.defaults.esp32s3`
- `partitions.csv`
- implementation files directly related to the task

## Layer Rules

- `AGENTS.md`: behavior rules, read order, resume / handoff protocol
- `resume-queue.md`: current pending work, default resume target, next concrete step
- `MEMORY.md`: stable repo-level working memory only
- `memory/YYYY-MM-DD.md`: daily working memory and checkpoints
- fact docs: topic-specific facts and baselines
- `decision-log.md`: append-only major engineering decisions
- `knowledge/`: evidence, experiments, logs, failure samples

Do not duplicate the same fact across multiple layers when one file is already
the stronger source of truth.

## Session Boundary

If the user says `开口`, or asks to continue from a prior session:

1. Read the files in `Read Order`
2. Resume from `resume-queue.md`
3. Only read deeper evidence if the current task requires it

If the user says `收口`, treat it as a handoff checkpoint:

1. Update `resume-queue.md`
2. Update today's `memory/YYYY-MM-DD.md` if needed
3. Promote stable conclusions to `MEMORY.md` when justified
4. Append to `decision-log.md` if a real decision was made

These two commands are not casual wording in this repo; they are the explicit
session control protocol.

## Checkpoint Rule

At any meaningful checkpoint:

1. Update `resume-queue.md`
2. Update today's daily memory if there is useful short-term carry-over
3. Promote only stable, cross-session conclusions to `MEMORY.md`
4. Keep detailed evidence out of the default resume path

## High-Value Capture Rule

Actively identify reusable high-value content during normal work and persist it
locally instead of leaving it only in chat context.

Treat the following as high-value by default:

- explicit engineering decisions
- repeated debugging conclusions
- stable cross-session reminders
- important constraints, boundaries, and interface expectations
- environment baselines and platform facts
- common failure modes and rollback paths
- conclusions that materially reduce future re-discovery cost

When such content appears:

1. choose the narrowest correct layer
2. write it once to the strongest source of truth
3. update `resume-queue.md` if it changes how the next session should resume
4. append to `decision-log.md` if it is a real decision
5. use `memory/YYYY-MM-DD.md` first when the conclusion is still fresh but not
   yet stable

Do not wait until the end of a long task to record reusable conclusions if they
are already clear.

## Interaction Contract

- Treat the markdown memory system itself as part of the durable collaboration
  surface, not just the technical project state.
- If the user asks to "沉淀" interaction rules, reopen / close protocol, or
  cross-session collaboration expectations, write them into the local memory
  layers instead of leaving them only in chat history.
- Prefer storing protocol in `AGENTS.md`, stable collaboration expectations in
  `MEMORY.md`, and session-specific interaction notes in `memory/YYYY-MM-DD.md`.
- When relevant work depends on verified context from sibling repositories or
  prior hardware bring-up, import the reusable conclusions into this repo's
  memory layers rather than assuming the next session will remember them.
- If the user switches from a sibling workspace such as `esp32` into this repo
  and wants to continue work, the handoff should feel seamless: recover the
  imported context from local memory first and do not make the user restate the
  same hardware / workflow background unless new evidence conflicts with it.

## Risk Boundary

- Do not treat chat history as durable memory
- Do not turn `MEMORY.md` into a dumping ground
- Do not put pending queue items into `MEMORY.md`
- Do not use `knowledge/` as the default resume entrypoint
- Prefer the narrowest valid source of truth
