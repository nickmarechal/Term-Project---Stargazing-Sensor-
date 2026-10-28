# Design Document (M2)

> **STATUS: M2 draft, 2026-10-27, written by Liam.** Nick was unavailable, so every decision in
> his area (storage, mechanism D, the record contract) is marked **proposed (Liam), Nick to
> confirm**. Values marked **(a)** are **assumed (datasheet), verify at bring-up**; the sensors are
> not wired yet, so no hardware value in this document is measured.
>
> At final submission this file must reflect the design **as it ended, not as it began**, with a
> short changelog of what M2's version got wrong.

## Architecture

Seven single-threaded processes (`1T`). A *rec* is one sealed 48-byte `struct dsm_record`
(`src/common/record.h`). The rates come from the sample periods: 5 s / 1 s / 10 s for sky
brightness, IR sky temperature, and ambient conditions, matching how fast each signal changes.

**Data path**

```
  TSL2591 @0x29 (a)          MLX90614 @0x5A (a)          BME280 @0x76 (a)       head at zenith
        └───────────────────────────┼────────────────────────────┘
                                    │ I2C bus 1, 3.3 V, 100 kHz (a): ≈2.6 transfers/s in total
════════════════════════════════════╪═════════════════════════════════════════════════ kernel
                i2c-bcm2835 driver + i2c-dev (/dev/i2c-1): one transfer at a time (a)
        ┌───────────────────────────┼────────────────────────────┐
        │ 0.4 xfer/s (a)            │ 2 xfer/s (a)               │ 0.2 xfer/s (a)    ioctl(I2C_RDWR)
════════╪═══════════════════════════╪════════════════════════════╪════════════════════ user
   ┌────▼─────┐                ┌────▼─────┐                 ┌────▼─────┐
   │ skyd  1T │                │cloudd 1T │                 │ envd  1T │
   └────┬─────┘                └────┬─────┘                 └────┬─────┘
        │ 0.2 rec/s → ring 1        │ 1 rec/s → ring 2           │ 0.1 rec/s → ring 3
        └───────────────────────────┼────────────────────────────┘
                                    │ 1.3 rec/s read · 1.3 sem_post/s     3 POSIX shm rings in
                                    │                                     /dev/shm, 1024 × 48 B
                                    │                                     each + 1 shared semaphore
                            ┌───────▼────────┐   AF_UNIX stream    ┌────────────┐
                            │  fusiond   1T  │◄───── on demand ───►│  dsq (CLI) │◄── user, over SSH
                            └───────┬────────┘                     └─────┬──────┘
                                    │ AF_UNIX SEQPACKET                  │ read-only open(),
                                    │ 1.37 rec/s ≈ 66 B/s                │ on demand
                            ┌───────▼────────┐                           │
              dskyd ───────►│  stored    1T  │                           │
          SEQPACKET 6 rec/h └───────┬────────┘                           │
                                    │ write() every 10 s (≈660 B) · fsync() every 60 s
════════════════════════════════════╪════════════════════════════════════╪════════════ kernel
                     page cache → ext4 → mmc driver → 16 GB microSD: ≈5.7 MB/day
```

Rates: 1.37 rec/s into `stored` = 1.3 SAMPLE + 4 FEATURE/min + ≥1 STATE/10 min, ≈5.7 MB/day
(computed). 6 HEARTBEAT/h = five children + `dskyd`. I2C transfer counts are our planned read
sequences, checked at bring-up (hardware-log §1–3). Every channel is `AF_UNIX`, a pipe, shared
memory or a signal: **no IP sockets**; the user reaches `dsq` over SSH. Intervals use
`CLOCK_MONOTONIC`, labels `CLOCK_REALTIME`; `fusiond` checks clock sync with `adjtimex()` 1/min
(no RTC until the DS3231 ordered 2026-10-27 is fitted; see Constraints).

| Process | Job (owners: see Ownership map) |
|---------|------------------------------------------------------------------------------|
| `dskyd` | Supervisor. Runs under systemd (`Restart=on-failure`); fork/execs the five children (`PR_SET_PDEATHSIG`). Per-child liveness pipe, ≥1 B / 5 s: EOF = died, 20 s silent = hung → `SIGKILL`. `SIGCHLD` + `waitpid()` exit code: crashed vs. sensor absent. Backoff 1→60 s; `DEAD` + re-probe every 60 s. Hourly `/proc/<pid>/statm`, `/stat` → HEARTBEAT to `stored`. All stderr → journald (≈6 lines/h) |
| `skyd` | TSL2591 → ring 1 every 5 s (a); 600 ms integration; gain stepped with hysteresis |
| `cloudd` | MLX90614 ambient + object → ring 2 every 1 s (a); PEC checked |
| `envd` | BME280 forced mode → ring 3 every 10 s (a); integer compensation |
| `fusiond` | Drains rings; sensor staleness; features, sun/moon, classifier, state machine; forwards SAMPLEs to `stored`; answers `dsq` |
| `stored` | Only writer of segments: batch, `fsync`, rotate, recover. `fusiond` buffers 4096 recs while it is down |
| `dsq` | Short-lived CLI: live state and health via `fusiond`; time-range dump from segments, read-only |

## Mechanism mapping

The user (PROBLEM.md) pays **30–45 min of setup or a 30–60 min drive** for every GOOD verdict
they act on, and the product's value is a **site baseline built over weeks**. Both mechanisms
come from those two facts.

**E: multi-process + IPC + supervisor.** *Component:* `dskyd`, one process per sensor, and
`fusiond`, over shm rings and `AF_UNIX` sockets. *Requirement:* **R-E1** a dead, hung or
unplugged sensor turns the verdict to UNKNOWN within **60 s** while the other sensors keep
sampling; **R-E2** a crashed process costs ≤ **10 s** of that sensor's data. *Why:* at a dark
site, overcast reads *darker* than clear, so a silent IR sensor produces a confident false GOOD,
the exact error that wastes the user's evening. In one process, a hung `ioctl` stalls every
sensor and a segfault kills them all. Separate processes keep the failure local: `dskyd`
restarts it, `fusiond` flags staleness after 3 missed periods (≤ 30 s), and the verdict degrades
honestly.

**D: custom crash-consistent storage.** *Component:* `stored` (`src/store/`) and the record
format (`src/common/`); policy numbers **proposed (Liam), Nick to confirm**. *Requirement:*
**R-D1** a power cut mid-write loses ≤ **60 s** of records and never an earlier one; **R-D2**
recovery is automatic on every boot, target **20/20** pulled-plug tests; **R-D3** ≥ **180 days**
of raw data plus all nightly rollups fit on the 16 GB card. *Why:* the baseline *is* the
product's memory, and a device on a yard extension cord sees brownouts (spec §17). Verdicts use
30-min windows, so losing ≤ 60 s (≤ 3 %) cannot flip a night's grade. That sets `fsync` at 60 s
rather than per record, with writes batched every 10 s to spare the card. CRC-sealed fixed-size
records make "truncate at the first bad record" a complete recovery rule.

**Isolation boundaries (E).** Each process boundary contains one failure: `skyd` — an I2C hang
in brightness must not stop cloud sensing; `cloudd` — the most failure-prone part (SMBus quirk),
isolated; `envd` — the humidity input, fails independently; `fusiond` — analysis can crash and
restart without stopping sampling; `stored` — a single writer rules out multi-writer corruption by
construction; `dskyd` — must outlive every child, and is itself restarted by systemd.

**Ring invariants (E).** One single-producer/single-consumer ring per sensor; `dskyd` creates the
shared memory before the fork. Chosen over sockets for **decoupled lifetimes**, not speed (1.3
rec/s is slow): samples keep queuing while `fusiond` restarts, and a sensor never blocks on it.

1. **Publish after write:** fill slot `head % N` with a sealed record, *then* release-store
   `head`; the reader acquire-loads `head`. A producer killed mid-copy never exposes the slot.
2. **CRC on read:** the reader checks `record_intact()` on its copy; a torn slot is counted and
   dropped.
3. **Never overwrite unread:** write only while `head − tail < N`; when full, drop the *new*
   sample and count an overrun (the `seq` gap proves it).
4. **Reader restart:** `tail` advances only after forwarding, so a killed `fusiond` may re-send
   one record; the duplicate (source, run, seq) is removed at query time.
5. **Lock-free:** `_Static_assert(ATOMIC_INT_LOCK_FREE == 2)`, because a lock-based atomic
   deadlocks if a process dies holding it. **Assumed, confirmed at build on the Pi.**

**Rejected.** **C** (real-time): sky brightness changes over minutes, so no path has a deadline.
**F** (no-drop high-rate): 1.3 rec/s total; a high-rate pipeline would be padding. **A** (driver)
is a stretch goal only if M3 lands early; **B** would only be honest with a pulse-output sensor
(TSL237) we did not buy.

## Failure-mode table

For each component: how it can fail, how the failure is detected, what the system does about it,
and what the log will show. **The 48-hour soak grades this table's honesty.**

| Component | How it fails | How we detect it | What the system does | What the log shows |
|---|---|---|---|---|
| TODO | TODO | TODO | TODO | TODO |

## Storage and data

What is stored, at what rate, in what format, with what retention, and **what happens to it when
the power dies mid-write.**

**TODO.**

## Constraints and substitutions

What the ideal build would use, what we are actually using, and what the gap costs: sensors
downgraded, data synthesized or replayed, claims narrowed.

> "A design document with nothing to report here has usually not met its hardware yet."

**TODO.** Already known: the budget forced a 2-sensor build, and single-unit retail pricing runs
well above the spec's $50–80 estimate.

## Evaluation plan

The measurements we will take (spec §9), each with its method and its target. Numbers committed
to now are twice as credible when hit later, and instructive either way.

**TODO.**

## Ownership map

Which partner owns which subsystems. Ownership means **first authorship and answerability at the
defense**, not exclusivity.

**TODO.** Must match `CLAUDE.md`.

## AI-use plan

What we'll use Claude Code for, what we won't, and how we keep the §3.3 boundary visible in the
repository — e.g. the product source tree contains no network client but the LAN interface, and
a grader can confirm that in one minute.

**TODO.**
