# Record format, data rates, and IPC

Detail behind `DESIGN.md` § Architecture, § Mechanism mapping and § Storage and data. The record
and header layouts are **implemented** (`src/common/record.h`, `src/store/store.h`). Everything
marked *proposed* in the record sections (the kinds, flags, `source_id`s and payloads, which
nothing depends on yet) was proposed by Liam and **confirmed by Nick on 2026-10-28**. The IPC section is Liam's
(mechanism E). The IPC is designed but not implemented.

## Record: 48 bytes, little-endian (implemented)

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 8 | `t_mono_ns` | `CLOCK_MONOTONIC`, for intervals |
| 8 | 8 | `t_real_ns` | `CLOCK_REALTIME`, the wall-clock label |
| 16 | 2 | `schema_ver` | `DSM_SCHEMA_VER` (1) |
| 18 | 2 | `source_id` | producing process (table below) |
| 20 | 2 | `kind` | record kind (table below) |
| 22 | 2 | `flags` | bit flags (table below) |
| 24 | 16 | `v[4]` | `int32` fixed-point values; meaning depends on `source_id` and `kind` |
| 40 | 4 | `seq` | per (source, run) counter |
| 44 | 4 | `crc32` | CRC-32/IEEE over bytes 0–43 |

The layout is enforced at compile time with `_Static_assert`. Rules: `record_init()` zeroes all
48 bytes first; `record_seal()` is called last; the store refuses unsealed records.

## Segment header: 24 bytes (implemented)

`magic "DSKY"` (4) · `schema_ver` (2) · `record_size` (2) · `created_real_ns` (8) · `reserved`
(4, always 0) · `crc32` over bytes 0–19 (4). On reopen, any mismatch means the file is refused
and left untouched.

## Kinds and flags

| `kind` | Value | | Flag | Bit |
|---|---|---|---|---|
| SAMPLE | 1 | | `VALID` | 0 |
| FEATURE | 2 | | `STALE` | 1 |
| STATE | 3 | | `SYNTHETIC` (all replayed or provoked data) | 2 |
| HEARTBEAT | 4 | | `CALIBRATING` (sensor warming up) | 3 |
| FAULT | 5 | | `CLOCK_UNSYNC` (*proposed*) | 4 |
| START (*proposed*) | 6 | | | |

0 is never a valid kind or `source_id`, so a zeroed record never looks real.

## Payloads (proposed)

| `source_id` | `kind` | `v[0]` | `v[1]` | `v[2]` | `v[3]` |
|---|---|---|---|---|---|
| 1 `skyd` | SAMPLE | TSL2591 CH0 count (full spectrum) | CH1 count (IR) | gain multiplier | integration, ms |
| 2 `cloudd` | SAMPLE | sky (object) temp, m°C | ambient (die) temp, m°C | 0 | 0 |
| 3 `envd` | SAMPLE | temperature, m°C | humidity, milli-%RH | pressure, Pa | 0 |
| 4 `fusiond` | FEATURE | feature id | value ×1000 | window, s | samples in window |
| 4 `fusiond` | STATE | state | rules fired (bitmask) | confidence ×1000 | 0 |
| 5 `dskyd` | HEARTBEAT | subject `source_id` | RSS, KiB | CPU time, ms | cumulative events |
| any (6 = `stored`) | FAULT | fault code | errno | consecutive count | 0 |
| any | START | run number (from `dskyd`) | 0 | 0 | 0 |

- **Raw counts, not lux,** for the TSL2591: lux means little at night-sky levels, and storing
  counts plus gain and integration time lets the brightness conversion be redone later.
- **MLX90614 in m°C:** its 0.02 K step is exactly 20 m°C (`m°C = raw × 20 − 273150`), so there
  is no rounding (datasheet scale, verify at bring-up).
- **FAULT vs. `!VALID`:** an I/O failure is a FAULT (the first one, then at most 1 per 60 s while
  it lasts); a successful read with an implausible value (saturated, MLX error bit) is a SAMPLE
  with `VALID` cleared.
- **`seq` per run:** each process start writes START with `seq` 0. A gap within a run is
  provable loss; the time between runs is the restart gap.

## Rate derivation (computed, not measured)

| Record | Rate | Records/day |
|---|---|---|
| `skyd` SAMPLE | 1 per 5 s | 17 280 |
| `cloudd` SAMPLE | 1 per 1 s | 86 400 |
| `envd` SAMPLE | 1 per 10 s | 8 640 |
| FEATURE | 4 per min | 5 760 |
| STATE | on change, ≥ 1 per 10 min | ≥ 144 |
| HEARTBEAT | 6 per h (5 children + `dskyd`) | 144 |
| FAULT, START | per event | ≈ 0 in steady state |
| **Total** | **≈ 1.37 rec/s** | **≈ 118 400 → 5.7 MB/day** |

- 180 days of raw data ≈ 1.0 GB, on a 16 GB card.
- `write()` every 10 s ≈ 13.7 records ≈ 660 B per call; `fsync()` 1440 per day.
- I2C transfers (planned read sequences; datasheet protocol, verify at bring-up): TSL2591 status
  + data = 2 per sample → 0.4/s; MLX90614 ambient + object = 2 → 2/s; BME280 trigger + burst = 2
  → 0.2/s. **≈ 2.6 transfers/s in total.**
- Buffer headroom: a 1024-slot ring holds 17 min of `cloudd`, 85 min of `skyd` and 170 min of
  `envd`; `fusiond`'s 4096-record buffer for `stored` holds ≈ 50 min.

## IPC (mechanism E)

### Sensor → `fusiond`: shared-memory rings

One single-producer/single-consumer ring per sensor daemon: 1024 slots × 48 B in POSIX shared
memory under `/dev/shm`, plus one shared semaphore that every producer posts and `fusiond` waits
on with a 1 s timeout. `dskyd` creates the shared memory before the fork and unlinks it on
orderly shutdown, so a ring outlives the restart of either side.

Chosen over sockets for **decoupled lifetimes, not speed** (1.3 rec/s is slow): samples keep
queuing while `fusiond` restarts, and a sensor never blocks on the hub.

**Invariants:**

1. **Publish after write:** fill slot `head % N` with an already-sealed record, *then* advance
   `head` with a release store. The reader acquire-loads `head` and reads only slots below it, so
   a producer `kill -9`'d mid-copy never exposes a half-written slot. The restarted producer
   reads `head` from shared memory and overwrites that same slot.
2. **CRC on read:** the reader copies the slot out and checks `record_intact()` before use. A
   torn slot (a bug, not the protocol) is counted and dropped, never trusted.
3. **Never overwrite unread:** the producer writes only while `head − tail < N`. When the ring is
   full, it drops the *new* sample and increments an overrun counter in the ring header; the
   `seq` gap proves the loss.
4. **Reader restart:** `tail` advances only after the record has been forwarded to `stored`. If
   `fusiond` is killed in between, the restarted `fusiond` re-sends that record. The duplicate
   has the same (source, run, seq) and is removed at query time.
5. **Lock-free across processes:** `_Static_assert(ATOMIC_INT_LOCK_FREE == 2)`, because a
   lock-based atomic would deadlock if a process died holding the lock. **Assumed, confirmed at
   build on the Pi** (64-bit ARM).

Edge cases: a producer killed between advancing `head` and posting the semaphore only delays
the record until the reader's next 1 s timeout.

### Other channels

| Channel | Kernel object | Rate | Notes |
|---|---|---|---|
| `fusiond` → `stored`, `dskyd` → `stored` | `AF_UNIX` `SOCK_SEQPACKET`, one 48-byte record per message | 1.37 rec/s; 6 rec/h | `stored` is the only writer. While it is down, `fusiond` buffers 4096 records (≈ 50 min), then drops the oldest and counts. **Proposed (Liam), confirmed (Nick) 2026-10-28** |
| `dsq` ↔ `fusiond` | `AF_UNIX` `SOCK_STREAM`, line-based request/response | on demand | `state`, `health`. `dsq` reads segments directly, read-only |
| `dskyd` ↔ each child | pipe (child holds the write end) + `SIGCHLD` | ≥ 1 B per 5 s | EOF = died; 20 s silent = hung → `SIGKILL` |
| all → log | stderr → journald | ≈ 6 lines/h steady + 1 per event | survives a `dskyd` crash |

### Supervision (`dskyd`)

- Runs under systemd with `Restart=on-failure`; children set `PR_SET_PDEATHSIG`, so they exit
  with it and a restarted `dskyd` starts a clean tree.
- After a crash: restart with 1 → 2 → 4 … 60 s backoff; 60 s of healthy running resets it.
- A sensor daemon that sees 3 consecutive failed reads exits with a distinct "sensor absent"
  code. `dskyd` marks it `DEAD` and re-probes every 60 s instead of restarting fast, so an
  unplugged sensor never causes a restart storm.
- 5 crashes within 300 s → `DEAD`, re-probe every 60 s; the rest of the system keeps running.
- Heartbeat: one HEARTBEAT per child within 60 s of start, then hourly. Contents: RSS and CPU
  time from `/proc/<pid>/statm` and `/stat`, plus cumulative events from the ring counters.
- Each child passes its run number in its START record (see `seq` above).
