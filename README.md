# Term-Project

This repository will stand as the work process for our term project.

CS 370 (Operating Systems) — Colorado State University, Fall 2026.
Prof. Shrideep Pallickara.

**Team:** Nick Marechal, Liam Sagal

> **Status: M1 → M2.** Concept locked (**Dark Sky Monitor**), problem memo written, sensors and
> Pi ordered 2026-10-06. Next: sensor bring-up (`docs/hardware-log.md`) and the M2 design document.

## What this will be

A Raspberry Pi with at least two cooperating sensors and software of our own, solving a specific
problem for a specific person. Constraints that hold regardless of which idea we pick:

- Two **physically distinct** sensors that genuinely cooperate — fused into one decision, not two
  demos stapled together.
- At least **two "menu" mechanisms** below the application layer, written by us and measured:
  a character driver, interrupt-driven input vs. polling, real-time scheduling, a custom
  crash-consistent storage layer, a multi-process architecture with a supervisor, or a no-drop
  high-rate sampling pipeline.
- **All intelligence is ours** — no LLM, no cloud inference, no pretrained models at runtime. The
  device works with the network cable unplugged.
- Must survive a **48-hour unattended soak** with hourly heartbeats and an injected fault.

## Building

```
make          # build
make test     # run tests
make asan     # build + run under AddressSanitizer/UBSan
make memcheck # run under valgrind
make clean
```

Targets run today; most are no-ops until there is source to build.

## Deploying to the Pi

```
make deploy PI=pi@<hostname>
```

Not yet configured — the Pi has not been ordered.

## Repository layout

```
CLAUDE.md          team law: constraints, ownership, workflow (graded deliverable)
PROBLEM.md         the problem memo (M1)
DESIGN.md          the design document (M2) — not yet written
EVALUATION.md      the evaluation report (M5) — not yet written
src/               systems core (C17)
tests/             tests
tools/             dev tooling; Python allowed here, no graded mechanism may live here
docs/              working notes
transcripts/       raw .jsonl Claude Code sessions, per partner, per milestone
```

## For a TA grading this

By final submission, this README must take you from a clean Raspberry Pi to a running system
without our help. It does not do that yet — the project is at M0.
