# term-project: team rules

Team law for CS 370 (Colorado State University, Prof. Shrideep Pallickara), Fall 2026.
Both partners maintain this file. Claude Code reads it every session, so **a constraint that
lives only in one partner's head protects nothing.**

This file is self-contained on purpose: a teammate who clones only this repo has everything
they need. (Nick has a separate personal course-notes folder one level up; that is not team law
and is not required to work in this repo.)

---

## Project status (update this as things get decided)

- **Idea: LOCKED — Dark Sky Monitor.** An offline device that measures sky brightness and cloud
  cover at one site and grades the night GOOD / MARGINAL / POOR / UNKNOWN with the evidence behind
  the verdict. Full design review in [`docs/design-review.md`](docs/design-review.md).
- **Team:** Nick Marechal + Liam Sagal.
- **Where we left off: [`docs/progress-log.md`](docs/progress-log.md). Read it first.** Newest
  entry on top: milestone position, what code exists and what it guarantees, the shared record
  contract, and the open work. Add an entry there when you finish a chunk of work.
- **Hardware: two kits.** **`darksky.local`** (Nick: Pi 4 Model B **1 GB** + TSL2591 + MLX90614
  **3.3 V** + BME280; arrived 2026-10-13, set up headless 2026-10-27, I2C on, power `0x0`) is the
  **device of record**: the soak, the demo and every evaluation number come only from it.
  **`starlight.local`** (Liam's dev Pi + the same three sensors) is for parallel development only.
  **`darksky.local`: all three sensors wired and answering 2026-10-28** (`0x29`, `0x5a`, `0x76`;
  BME280 genuine). `starlight.local` not wired. DS3231 RTC to be ordered; until it is fitted the Pi
  takes real time from the network at boot. Enclosure still unbought. See
  [`docs/hardware-log.md`](docs/hardware-log.md) and [`docs/bill-of-materials.md`](docs/bill-of-materials.md).
- **Mechanisms: D** (crash-consistent storage) **+ E** (multiprocess + IPC + supervisor). C and F
  were considered and rejected — at 0.1–1 Hz sampling neither is justified, and being able to say
  why is a stronger defense answer than bolting them on.
- **Design: `DESIGN.md` M2 written 2026-10-27 (Liam).** The decisions in Nick's area (listed in
  the 2026-10-27 M2 entry of [`docs/progress-log.md`](docs/progress-log.md)) were confirmed by
  Nick on 2026-10-28. Values marked (a) are datasheet assumptions,
  not measurements. Record, IPC and supervision detail: [`docs/record-and-ipc.md`](docs/record-and-ipc.md).
- **Next action: M3, per [`docs/m3-plan.md`](docs/m3-plan.md)** (Canvas closes Fri 2026-10-30,
  8 pm MT). It says who does what, the interface between the storage and sensor sides, and the
  steps for each. Hardware-log §3–5 on `darksky.local` are still open after that.
- **Open gap:** the problem memo names an archetype, not a named individual. Outreach to the
  Northern Colorado Astronomical Society / CSU astronomy faculty still pending.

## Commands

- Build: `make`            Deploy to Pi: `make deploy PI=pi@<host>`
- Tests: `make test`       Sanitizers: `make asan`      Valgrind: `make memcheck`
- Soak sanity: `make soakcheck` (1-hour miniature of the 48h run)
- **A change is DONE only when build, test, and asan pass. Show the output.**

Targets exist and run today; several are no-ops until there is source to build. Do not let a
target silently become a lie — if it stops doing what its name says, fix it or rename it.

## Hard constraints (graded — spec §3.3, §3.4)

- The PRODUCT makes **no network calls except serving its own LAN interface**. No LLM APIs, no
  cloud inference, no pretrained models, no external weather/data APIs at runtime. The device
  must work with the network cable unplugged. The intelligence in `analysis/` is ours. A grader
  must be able to confirm this in one minute by reading the source tree.
  - Public data (NWS, USGS, etc.) may be used **only** as an offline ground-truth comparison
    while writing the evaluation report — never called from the device's own code.
- Every daemon must be **supervisable**: clean exit codes, no orphaned fds across restart,
  heartbeat within 60s of start.
- Every allocation checked; every syscall's error path handled and logged. The 48-hour soak is
  the test suite of last resort.
- **NEVER weaken, skip, or delete a test to make the suite pass.**
- Replayed or synthesized data is **labeled as such everywhere it appears** — in logs, in the
  report, in the demo. The 48h soak and the live demo run on live sensors.

## Style

- Systems core: **C17, `-Wall -Wextra -Werror`, no VLAs.** `goto`-cleanup for multi-resource
  functions. ASan/valgrind findings cap that component's score at 50%, so they are bugs, not
  warnings.
- Python permitted only under `tools/` and `ui/`. **No graded mechanism may live there.**
- Smallest diff that passes. Do not refactor unrelated code.

## Ownership

Ownership means **first authorship and answerability at the defense**, not exclusivity. Each
partner is individually examined on their own subsystems plus one question from across the
boundary.

**Changed for M3 on a call, 2026-10-28 (both agreed):** sensors moved to Nick, because only
`darksky.local` is wired; the storage side from M3 on moved to Liam, because `stored`'s socket
runs only on Linux, which is what Liam develops on. Details: [`docs/m3-plan.md`](docs/m3-plan.md).

- **Nick:** `src/common/` (record format, CRC, logger) and the store library through crash
  recovery (`src/store/store.{c,h}` up to `bc6d3bc`); **`src/sensors/`** (I2C layer, conversion
  math, `sensorcheck`, `skyd`, `cloudd`, `envd`); the header `src/store/store_client.h` (the
  interface between the two sides). Analysis: features, ephemeris, and `dsq`'s query commands.
- **Liam:** **`src/store/` from M3 on** (`fsync`, counters, read-only scan, batching, `stored`,
  `store_client.c`), **`dsq dump`** (`src/dsq/`), `tests/itest_stored.c`; then mechanism **E** at
  M4: `src/super/` (`dskyd`), the shm rings, `fusiond`'s ring-draining loop. Analysis:
  classifier + state machine.
- Mechanism **D** is shared: the format and recovery are Nick's, the daemon that runs them is
  Liam's. Each answers for his own part at the defense.
- **Linux-only code:** `src/sensors/` I2C and daemons (`<linux/i2c-dev.h>`), `stored` and
  `store_client` (`SOCK_SEQPACKET` fails on macOS). Everything else builds and tests on a Mac.
- The agent edits outside the current session owner's area only when told explicitly whose
  session this is.

## Workflow (spec §10, Assignment 0's nine steps apply verbatim)

- Multi-file or algorithmic change: **plan first, wait for approval.** No code in the first pass.
- **Hardware bugs: paste real evidence** — `dmesg`, timing captures, `/proc/interrupts`, logic
  analyzer output. Never propose or accept a fix derived from a verbal description of a sensor
  problem. The agent will confidently reason about a sensor it has never met and be wrong in
  ways only an instrument catches.
- Commit only from a green state; message format `M<n>: <what>`.
- **≥ 40 meaningful commits** across the project, both partners well represented. A pair of
  "final submission" commits is an automatic process grade of **zero, for both partners.**
- Each partner works in **their own clone and their own Claude Code sessions.** Working in one
  shared login defeats the per-partner transcript grading and costs both of you.
- Every milestone's diffs get **two reviews**: a fresh-context agent review and a human review
  by the partner who did not write the code. Each partner's `PROMPTLOG.md` must record at least
  one review of the other's work.

## Transcript discipline (do not skip — spec §6, §10.4)

At **every** milestone M1–M5, each partner copies their raw `.jsonl` sessions from
`~/.claude/projects/<project-dir>/` into `transcripts/<partner>/`. As-is. Not summarized, not
exported, not retyped. See `transcripts/README.md`.

Local transcripts purge after 30 days by default and this project spans more than 30 days.
Losing Week-5 transcripts in Week 15 is a foreseeable loss, and foreseeable losses are not
excused.
