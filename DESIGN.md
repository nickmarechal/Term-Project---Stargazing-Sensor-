# Design Document (M2)

> **M2, 2026-10-27: drafted by Liam (Nick unavailable); the decisions in Nick's area were confirmed by Nick on 2026-10-28.**
> **(a)** = **assumed (datasheet), verify at bring-up**: no sensor is wired yet, so nothing here
> is measured.

## Architecture

Seven single-threaded (`1T`) processes; *rec* = one sealed 48-byte record; sample periods 5/1/10 s.

```
  TSL2591 @0x29 (a)          MLX90614 @0x5A (a)          BME280 @0x76 (a)
        └───────────────────────────┼────────────────────────────┘
                                    │ I2C bus 1, 3.3 V, 100 kHz (a): ≈2.6 transfers/s in total
════════════════════════════════════╪════════ kernel: i2c-bcm2835 + i2c-dev, /dev/i2c-1 (a) ══
        ┌───────────────────────────┼────────────────────────────┐
        │ 0.4 xfer/s (a)            │ 2 xfer/s (a)               │ 0.2 xfer/s (a)    ioctl(I2C_RDWR)
════════╪═══════════════════════════╪════════════════════════════╪════════════════════ user
   ┌────▼─────┐                ┌────▼─────┐                 ┌────▼─────┐
   │ skyd  1T │                │cloudd 1T │                 │ envd  1T │
   └────┬─────┘                └────┬─────┘                 └────┬─────┘
        │ 0.2 rec/s → ring 1        │ 1 rec/s → ring 2           │ 0.1 rec/s → ring 3
        └───────────────────────────┼────────────────────────────┘   shm, 1024 × 48 B each
                                    │ 1.3 rec/s read · 1.3 sem_post/s
                            ┌───────▼────────┐   AF_UNIX stream    ┌────────────┐
                            │  fusiond   1T  │◄───── on demand ───►│  dsq (CLI) │◄── user, over SSH
                            └───────┬────────┘                     └─────┬──────┘
                                    │ AF_UNIX SEQPACKET 1.37 rec/s       │ read-only, on demand
                            ┌───────▼────────┐                           │
              dskyd ───────►│  stored    1T  │                           │
          SEQPACKET 6 rec/h └───────┬────────┘                           │
                                    │ write() every 10 s (≈660 B) · fsync() every 60 s
════════════════════════════════════╪════════════════════════════════════╪════════════ kernel
                     page cache → ext4 → mmc driver → 16 GB microSD: ≈5.7 MB/day
```

Every channel is `AF_UNIX`, a pipe, shm or a signal: **no IP sockets**. **Supervision:** `dskyd`
(under systemd) fork/execs the five children, watches a per-child liveness pipe (EOF = died, 20 s
silent = hung → `SIGKILL`), restarts with 1→60 s backoff, parks an absent sensor as `DEAD` with a
60 s re-probe, and logs hourly HEARTBEATs; stderr → journald. Rates and channel details:
[`docs/record-and-ipc.md`](docs/record-and-ipc.md).

## Mechanism mapping

Each GOOD acted on costs the user **30–45 min of setup or a 30–60 min drive**; the product's value
is a **site baseline built over weeks** (PROBLEM.md).

**E: multi-process + IPC + supervisor** (`dskyd`, one process per sensor, `fusiond`; shm rings,
`AF_UNIX` sockets). **R-E1:** a dead, hung or unplugged sensor turns the verdict UNKNOWN within
**60 s** while the others keep sampling. **R-E2:** a crash costs ≤ **10 s** of that sensor's
data. *Why:* at a dark site, overcast reads *darker* than clear, so a silent IR sensor yields a
confident false GOOD, the error that wastes the evening. In one process, one hung `ioctl` stalls
every sensor; separate processes keep failures local, `fusiond` flags staleness after 3 missed
periods (≤ 30 s), and analysis restarts without stopping sampling.

**D: crash-consistent storage** (`stored`, record format). **R-D1:** a power cut loses
≤ **60 s** of records, never an earlier one. **R-D2:** automatic recovery, **20/20** pulled-plug
tests. **R-D3:** ≥ **180 days** raw plus all rollups fit the 16 GB card. *Why:* the baseline *is*
the product's memory, and a yard extension cord sees brownouts (spec §17). Verdicts use 30-min
windows, so losing ≤ 60 s (≤ 3 %) can't flip a night: hence `fsync` every 60 s, with 10 s write
batching to spare the card; CRC-sealed fixed-size records make truncation a complete recovery.

**Ring invariants (E)** ([full](docs/record-and-ipc.md)): chosen for **decoupled lifetimes, not
speed**. Publish-after-write (release/acquire on `head`), CRC check on read, never overwrite
unread (`head − tail < N`), reader-restart duplicates removed by (source, run, seq), lock-free
`_Static_assert` (**assumed, confirmed at build on the Pi**). **Rejected:** C, F (nothing at 1.3
rec/s has a deadline or rate to sustain); A is a stretch; B needs a pulse-output sensor.

## Failure-mode table

Each row names its injection (spec §9). Log lines, shown from LEVEL on, are the **planned format
(`<ISO time> <mono s> <proc>[pid] LEVEL event k=v`), not captured output**.

| Component | Failure (injected by) | Detected by → system does | Log shows |
|----------|----------------|--------------------------------------------|------------------------|
| sensor daemon | unplugged (pull jumper) | 3 failed reads, errno `EREMOTEIO`/`ENXIO` (a) → FAULT, exit "absent"; `dskyd`: `DEAD`, re-probe 60 s; UNKNOWN | `ERROR cloudd read_fail errno=EREMOTEIO n=3` |
| sensor daemon | hung (`kill -STOP`) | 20 s without a liveness byte → `SIGKILL`, restart | `WARN dskyd envd hung action=SIGKILL` |
| any child | crash (`kill -9`) | pipe EOF + `SIGCHLD` → restart, backoff 1→60 s, new run; rings hold ≥17 min for `fusiond`; `fusiond` buffers 4096 recs for `stored` | `WARN dskyd skyd died sig=9 run=4` |
| any child | storm (5× `kill -9` in 5 min) | → `DEAD`, re-probe 60 s; the rest keeps running | `ERROR dskyd cloudd storm state=DEAD` |
| I2C bus | all lost (pull head's 3.3 V) | all absent ≤ 30 s → UNKNOWN "no sensors"; a real lockup can't be injected safely and takes this path | `ERROR dskyd all_sensors_absent` |
| reading | saturated (flashlight) | TSL2591 range check → SAMPLE without `VALID`, ignored by `fusiond` | `WARN skyd saturated ch0=65535` |
| IR window | dew (breathe; `SYNTHETIC`) | dew margin < 1 °C + ΔT collapse, brightness steady (a) → "likely condensation, not cloud" | `WARN fusiond condensation` |
| `stored` | power cut (pull plug) | boot CRC scan → truncate at the first bad record | `WARN stored recover cut_bytes=31` |
| `stored` | `remount,ro`; ballast to < 2 GB | write error → exit, recover on restart; `statvfs()` → raw off, rollups on | `ERROR stored write_fail errno=EROFS` |
| `dskyd` | crash (`kill -9`) | systemd → children exit (`PDEATHSIG`); tree restarts in 1 s | `dskyd.service: Main process exited` |
| clock | no RTC or network (offline reboot) | `adjtimex()` unsynced → `CLOCK_UNSYNC`, UNKNOWN; sampling continues | `WARN fusiond clock_unsync` |
| any process | slow leak (soak; not injectable) | hourly RSS in HEARTBEAT → trend | `INFO dskyd hb fusiond rss_kb=812` |
| `dsq` | query, `fusiond` down | `connect()` fails → "UNKNOWN (hub down)"; history readable; exit 2 | `state: UNKNOWN (hub down)` |

## Storage and data

Decided, not yet built, except parts marked *implemented*; details in [`docs/record-and-ipc.md`](docs/record-and-ipc.md).

- **Record** (*implemented*): 48 B, little-endian, CRC-32-sealed. Kinds: SAMPLE (raw fixed-point
  values), FEATURE/STATE, HEARTBEAT, FAULT, START. Failed read → FAULT; implausible value →
  SAMPLE without `VALID`; replayed or provoked → `SYNTHETIC`. `seq` counts per (source, run),
  each run opened by START, so a gap within a run is provable loss. **≈5.7 MB/day, ≈1.0 GB per
  180 days** (computed; checked after the first 24 h run).
- **Files, durability, retention:** `data/seg-NNNNNN.dsm` = 24 B header (*implemented*) +
  records, one per night, rotated at local noon (24 h monotonic if unsynced), ≤ 32 MB. `write()`
  every 10 s or 64 recs; `fsync()` every 60 s, at rotation and shutdown, plus the directory on
  segment creation. Raw kept 180 days, nightly rollups forever; < 2 GB free → raw writes stop.
- **Power cut mid-write:** ≤ 60 s lost. On start, `stored` CRC-checks each slot of the open
  segment (closed ones were `fsync()`ed), truncates at the first bad one (torn or zero tail; zeros
  never pass, *tested*) and logs counts; a 1–23 B file named like our segment is a torn header,
  recreated. *Implemented* (116 checks, ASan/valgrind clean): header checks, refusing foreign
  files and unsealed records, stop after a failed write. M3: recovery, batching, `fsync`.

## Constraints and substitutions

| Area | Ideal → actual | What the gap costs |
|------------|-------------------------------------------|------------------------------------------|
| Brightness | calibrated SQM → TSL2591, no reference | **relative to the site baseline**, never absolute mag/arcsec² |
| Cloud | commercial sensor → MLX90614 + BME280, own humidity fit | if humid-clear and overcast don't separate, the claim narrows to dry nights |
| Hardware | Adafruit, 2 GB Pi, endurance card, DS3231, enclosure → clones, 1 GB Pi 4, standard 16 GB, no RTC (to be ordered), no enclosure | BME280 may be a BMP280 (hardware-log §2); MLX FOV unknown; offline reboot → UNKNOWN until network time; no outdoor runs; Pi, card fine (a) |
| Kits | one → two: `darksky.local` (Nick), `starlight.local` (Liam) | soak, demo, all evaluation numbers **only from `darksky.local`**; kit differences in hardware log |
| User, truth | named astronomer, months of labels → archetype; obs. log + `SYNTHETIC` provoked data | requirements from PROBLEM.md; counts, not %, on small n |
| Budget | $50–80 per team (spec §17) → Nick ≈ $68 (Pi $35 + sensors ≈ $33), Liam $77 (Pi $45 + sensors $32): **team ≈ $145** | **≈ $65 over the guideline's top (≈ 1.8×)**, before unrecorded PSUs, SD cards, RTC, enclosure; each kit alone is within it |

**Known gap (2026-10-27): "both sensors electrically alive" is not met** (no kit wired). Plan:
run hardware-log §1–5 on `darksky.local` by 2026-11-03; correct the (a) values from evidence.

## Evaluation plan

Targets are commitments, not results. (a) = hardware-dependent; loaded = `stress-ng --cpu 4`.

| # | Measurement | Method | Target |
|-|----------------|------------------------------------------|---------------------------|
| 1 | Sample → hub latency | `t_mono` at read vs. ring dequeue; 24 h idle + 24 h loaded; distributions | p99 ≤ 50 ms idle, ≤ 200 ms loaded (a) |
| 2 | Sample → durable | read time vs. the covering `fsync()` | p99 ≤ 61 s, by design |
| 3 | Unplug → UNKNOWN | pull a sensor 20×; last good sample → STATE | max ≤ 60 s (R-E1) |
| 4 | CPU, RSS per process | hourly HEARTBEAT over the 48 h soak, plotted | total < 5 % of a core; RSS ≤ 8 MB each; growth ≤ 1 % after hour 1 (a) |
| 5 | **Domain:** cloud state | manual sky observation (okta) logged at the same time as the device reading; ΔT_corr class (clear / broken / overcast) vs. it; confusion matrix | ≥ 80 % agreement on ≥ 40 obs. |
| 6 | **Domain:** night verdict | GOOD/MARGINAL/POOR vs. the observer's verdict, as N of M | ≥ 70 % on ≥ 20 obs. |
| 7 | **E:** `kill -9` | 20 kills × 5 children, scripted | detect ≤ 1 s; new sample ≤ 10 s (R-E2); no loss elsewhere |
| 8 | **E:** hang | `kill -STOP` each sensor daemon 10× | `SIGKILL` at 20 ± 1 s; UNKNOWN ≤ 60 s |
| 9 | **E:** isolation cost | interval jitter, 1 vs. 3 daemons on the bus, idle and loaded | p99 ≤ 50 ms (a); > 100 ms → I2C broker |
| 10 | **D:** `fsync` sweep | 1 / 10 / 60 / 300 s; card writes (`/proc/diskstats`), `fsync()` time distribution | ≤ 30 MB/day at 60 s (a); full curve |
| 11 | **D:** power cut | pull the plug mid-write ≥ 20×, outside the soak | 20/20 automatic; ≤ 60 s lost each (R-D1, R-D2) |
| 12 | Ephemeris | sun and moon altitude vs. published almanac, ≥ 50 dates | ≤ 0.1° sun, ≤ 0.5° moon |
| 13 | Fault injection | every failure-table row, as listed | expected log line; recovery (or held `DEAD`), no manual step |
| 14 | 48 h soak (spec §3.4) | live sensors, hourly heartbeat, ≥ 1 unplug and ≥ 1 `kill -9` | 48 h; no unplanned restarts; no `seq` gaps outside injected faults |

## Ownership map

First authorship and defense answerability; each reviews the other's diffs (matches `CLAUDE.md`).
**Liam (E):** `src/super/` (`dskyd`), `src/sensors/` (`skyd`, `cloudd`, `envd`, I2C layer), IPC
(rings, sockets, pipes, `fusiond`'s ring loop); classifier + state machine. **Nick (D):**
`src/common/`, `src/store/` (`stored`, recovery, rollups); features, ephemeris, `dsq`. **Both:**
Makefile, tests, `CLAUDE.md`, docs, the device of record.

## AI-use plan

- **Claude Code is used for** planning, reviews of each milestone's diffs, tests, drafts we review
  line by line, and docs, in each partner's own sessions.
- **Never in the product:** no LLM, cloud inference or pretrained model at runtime (spec §3.3);
  thresholds come from our own data. No hardware fix without pasted evidence.
- **Checkable boundary:** the product is C under `src/` (Python only in `tools/`) and opens **no
  IP sockets**; a future LAN interface would be one listen-only file. This prints nothing:
  `grep -rnE 'AF_INET|PF_INET|getaddrinfo|gethostbyname|curl|https?://' src/`. Only the OS's
  clock sync uses the network (until the DS3231); offline, the product reports UNKNOWN.
