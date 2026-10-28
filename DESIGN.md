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

Rates are computed from the sample periods (derivation in
[`docs/record-and-ipc.md`](docs/record-and-ipc.md)). Every channel is `AF_UNIX`, a pipe, shared
memory or a signal: **no IP sockets**; the user reaches `dsq` over SSH. `fusiond` checks clock
sync with `adjtimex()` 1/min (no RTC yet; see Constraints).

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

**Isolation (E):** one failure domain per process. A hang in one sensor (the MLX90614's SMBus
quirk is the likeliest) can't stop the others; analysis restarts without stopping sampling;
`stored` is the single writer; `dskyd` outlives them all.

**Ring invariants (E).** Shared-memory rings were chosen for **decoupled lifetimes, not speed**:
samples keep queuing while `fusiond` restarts. The rules: publish-after-write (release/acquire on
`head`), CRC check on read, never overwrite unread (`head − tail < N`), duplicates after a reader
restart removed by (source, run, seq), and a lock-free `_Static_assert` (**assumed, confirmed at
build on the Pi**). Full statement in [`docs/record-and-ipc.md`](docs/record-and-ipc.md).

**Rejected:** C and F (at 1.3 rec/s nothing has a deadline or a rate to sustain); A is a stretch
goal; B needs a pulse-output sensor we didn't buy.

## Failure-mode table

Every row names how we will inject it, so EVALUATION.md can report each one (spec §9). Log lines
follow `<ISO time> <mono s> <proc>[pid] <LEVEL> <event> k=v…`; the column shows LEVEL onward.
They are **examples of the planned format, not captured output**. ‡ = **proposed (Liam), Nick to
confirm**.

| Component | Failure (injected by) | Detected by → system does | Log shows |
|-------------|---------------|-----------------------------------------|----------------------|
| sensor daemon | unplugged (pull its jumper) | 3 failed reads, errno `EREMOTEIO`/`ENXIO` (a) → FAULT rec, exit "absent"; `dskyd`: `DEAD`, re-probe every 60 s; verdict UNKNOWN | `ERROR cloudd read_fail errno=EREMOTEIO n=3` |
| sensor daemon | hung (`kill -STOP`) | no liveness byte for 20 s → `SIGKILL`, restart | `WARN dskyd envd hung action=SIGKILL` |
| any child | crash (`kill -9`) | pipe EOF + `SIGCHLD` → restart with 1→60 s backoff, new run. Meanwhile rings hold ≥17 min for `fusiond`, and `fusiond` buffers 4096 recs for `stored` ‡ | `WARN dskyd skyd died sig=9 run=4` |
| any child | restart storm (5× `kill -9` in 5 min) | → `DEAD`, re-probe every 60 s; the rest keeps running | `ERROR dskyd cloudd storm state=DEAD` |
| I2C bus | all sensors lost (pull the head's 3.3 V wire) | all daemons absent within 30 s → UNKNOWN "no sensors". A real bus lockup can't be injected safely; it takes this path | `ERROR dskyd all_sensors_absent` |
| reading | saturated (flashlight on TSL2591) | range check → SAMPLE without `VALID`, ignored by `fusiond` | `WARN skyd saturated ch0=65535` |
| IR window | dew (breathe on it; `SYNTHETIC`) | dew margin < 1 °C + ΔT collapse while brightness holds (a) → "likely condensation, not cloud" | `WARN fusiond condensation` |
| `stored` ‡ | power cut mid-write (pull the plug) | boot-time CRC scan → truncate at the first bad record | `WARN stored recover cut_bytes=31` |
| `stored` ‡ | write error (`remount,ro`); disk < 2 GB (ballast file) | syscall error → exit, recover on restart; `statvfs()` → raw writes off, rollups continue | `ERROR stored write_fail errno=EROFS` |
| `dskyd` | crash (`kill -9`) | systemd → children exit (`PR_SET_PDEATHSIG`); the tree restarts in 1 s | `dskyd.service: Main process exited` |
| clock | no RTC, no network (power-cycle offline) | `adjtimex()` unsynchronized → `CLOCK_UNSYNC` flag, verdict UNKNOWN; sampling continues | `WARN fusiond clock_unsync` |
| any process | slow leak (not injectable; watched over the soak) | hourly RSS in HEARTBEAT → a visible trend | `INFO dskyd hb fusiond rss_kb=812` |
| `dsq` | queried while `fusiond` is down | `connect()` fails → "UNKNOWN (hub down)"; history still readable; exit 2 | `state: UNKNOWN (hub down)` |

## Storage and data

Everything in this section is **proposed (Liam), Nick to confirm**, except where marked
*implemented*.

- **Record** (*implemented*, `src/common/record.h`): 48 bytes, little-endian. Fields: two
  timestamps, schema, `source_id`, `kind`, `flags`, four `int32` fixed-point values, `seq`, and
  a CRC-32 over the first 44 bytes. **≈1.37 rec/s ≈ 5.7 MB/day ≈ 1.0 GB per 180 days**
  (computed; checked against real file sizes after the first 24 h run).
- **Segments:** `data/seg-NNNNNN.dsm` = a 24-byte header (*implemented*) + records. One per
  night, rotated at local noon (every 24 h of monotonic time if the clock is unsynced), capped at
  32 MB. `data/rollup/` holds one summary per night.
- **Durability:** `write()` every 10 s or 64 recs; `fsync()` every 60 s, at rotation and at
  shutdown; directory `fsync()` after creating a segment. **Retention:** raw 180 days, rollups
  forever; below 2 GB free, raw writes stop (logged) and rollups and heartbeats continue.

- **Contents** (field layout, payloads and full rate derivation in
  [`docs/record-and-ipc.md`](docs/record-and-ipc.md)): SAMPLEs carry raw sensor values in
  fixed-point (TSL2591 counts + gain, temperatures in m°C, humidity, pressure); `fusiond` writes
  FEATURE and STATE; `dskyd` writes HEARTBEAT; any process writes FAULT and START. A failed read
  is a **FAULT**; an implausible value is a **SAMPLE without `VALID`**; replayed or provoked data
  carries `SYNTHETIC`. `seq` counts per (source, run), and each run begins with a START record,
  so a gap within a run is provable loss.

**Power cut mid-write.** At most the last 60 s are lost (R-D1). The open segment may end in a
torn record or in zeros, and an all-zero record never passes its CRC (*tested*). On start,
`stored` checks each 48-byte slot of the open segment, truncates at the first bad one,
`fsync()`s, and logs counts kept and cut. Closed segments were `fsync()`ed at rotation and are
not rescanned. A 1–23-byte file with our segment name in our directory is a torn header and is
recreated; any other short file is refused. `fusiond`-restart duplicates are removed at query
time by (source, run, seq). *Implemented today* (116 checks, ASan and valgrind clean): header
checks, refusal of foreign files and unsealed records, stopping after a failed write. *M3:*
recovery, batching, `fsync`, rotation, rollups.

## Constraints and substitutions

| Area | Ideal → actual | What the gap costs |
|------------|-------------------------------------------|------------------------------------------|
| Brightness | calibrated sky-quality meter → TSL2591, no reference SQM | brightness is **relative to the site baseline**, never absolute mag/arcsec² |
| Cloud | commercial cloud sensor → MLX90614 + BME280, humidity correction fitted from our data | if humid-clear and overcast don't separate, the cloud claim narrows to dry nights |
| Hardware | Adafruit parts, Pi 4 2 GB, 32 GB high-endurance card → Amazon clones (GY-906 etc.), Pi 4 **1 GB**, standard 16 GB card | BME280 may be a BMP280 (hardware-log §2); MLX90614 field of view unknown; no Pi or card cost expected at ≈5.7 MB/day (a) |
| Kits | one device → **two sensor kits** (one per partner) for parallel development | soak, demo and every evaluation number come **only from the device of record, `darksky.local` (Nick's Pi)**; parts differences between kits are logged in the hardware log |
| Clock | DS3231 RTC → none until the one ordered 2026-10-27 arrives | after an offline power cut, verdicts are UNKNOWN until network time |
| Enclosure | weatherproof head, LDPE window, dew heater → none yet (hardware-log §4) | no unattended outdoor runs until built; dew detected, not prevented |
| User, ground truth | named astronomer, months of labeled nights → archetype (outreach pending); observation log 2–3×/night + provoked and replayed data (`SYNTHETIC`) | requirements come from PROBLEM.md, not interviews; few clear November nights, so counts, not percentages, on small n |
| Budget | spec §17: $50–80 per team → two kits: Nick's ≈ $68 (Pi + sensors; PSU, card, reader unrecorded), Liam's not yet recorded | over budget with two kits plus RTC and enclosure; the second kit buys parallel bring-up, not a better device |

**Known gap (2026-10-27): "both sensors electrically alive" is not met.** The device of record
is set up (hardware-log §0), but no sensors are wired on either kit, so every (a) value is
unverified. Plan: wire and run hardware-log §1–5 on `darksky.local`, target 2026-11-03, and
correct this document from the evidence.

## Evaluation plan

Targets are commitments, not results. (a) = depends on hardware not yet measured. ‡ =
**proposed (Liam), Nick to confirm**. "Loaded" = `stress-ng --cpu 4`.

| # | Measurement | Method | Target |
|-|----------------|------------------------------------------|---------------------------|
| 1 | Sample → hub latency | `t_mono` at read vs. at ring dequeue; 24 h idle + 24 h loaded; distributions | p99 ≤ 50 ms idle, ≤ 200 ms loaded (a) |
| 2 | Sample → durable latency ‡ | read time vs. the `fsync()` covering it | p99 ≤ 61 s, by design |
| 3 | Unplug → UNKNOWN | pull a sensor 20×; last good sample → STATE record | max ≤ 60 s (R-E1) |
| 4 | CPU and RSS per process | hourly HEARTBEAT over the 48 h soak, plotted | total < 5 % of one core; each RSS ≤ 8 MB; growth ≤ 1 % after hour 1 (a) |
| 5 | **Domain:** cloud-state accuracy | ground truth: a manual sky observation (cloud cover in okta), logged with its time alongside the device reading at that moment; device ΔT_corr class (clear / broken / overcast) vs. that log; confusion matrix | ≥ 80 % agreement on ≥ 40 observations |
| 6 | **Domain:** night verdict | GOOD/MARGINAL/POOR vs. the observer's verdict; reported as N of M | ≥ 70 % on ≥ 20 observations |
| 7 | **E:** `kill -9` recovery | 20 kills of each of 5 children, scripted | detection ≤ 1 s; first new sample ≤ 10 s (R-E2); no loss in other sensors |
| 8 | **E:** hang recovery | `kill -STOP` each sensor daemon 10× | `SIGKILL` at 20 ± 1 s; UNKNOWN ≤ 60 s |
| 9 | **E:** cost of isolation | sample-interval jitter, 1 vs. 3 daemons on the bus, idle and loaded | p99 jitter ≤ 50 ms (a); > 100 ms → switch to the I2C broker |
| 10 | **D:** `fsync` sweep ‡ | 1 / 10 / 60 / 300 s; card writes from `/proc/diskstats`, `fsync()` duration distribution | ≤ 30 MB/day written at 60 s (a); the full curve reported |
| 11 | **D:** power-cut recovery ‡ | pull the plug mid-write ≥ 20×, outside the soak | 20/20 automatic; ≤ 60 s lost each time (R-D1, R-D2) |
| 12 | Ephemeris ‡ | sun and moon altitude vs. published almanac, ≥ 50 dates | ≤ 0.1° sun, ≤ 0.5° moon |
| 13 | Fault injection | every failure-table row, injected as listed | expected log line present; recovery (or held `DEAD`) with no manual step |
| 14 | 48 h soak (spec §3.4) | live sensors; hourly heartbeat; ≥ 1 unplug and ≥ 1 `kill -9` | 48 h; no unplanned restarts; no `seq` gaps outside injected faults |

## Ownership map

Which partner owns which subsystems. Ownership means **first authorship and answerability at the
defense**, not exclusivity.

**TODO.** Must match `CLAUDE.md`.

## AI-use plan

What we'll use Claude Code for, what we won't, and how we keep the §3.3 boundary visible in the
repository — e.g. the product source tree contains no network client but the LAN interface, and
a grader can confirm that in one minute.

**TODO.**
