# Progress log

Where the project stands, written so that either partner, or a fresh Claude Code session, can pick
up cold. **Read this after `CLAUDE.md` and before starting work.** Newest entry first. Facts and
evidence only; plans live in `DESIGN.md` and `docs/design-review.md`.

When you finish a chunk of work, add an entry at the top: date, who, what changed, how it was
verified, and what it leaves open.

---

## 2026-10-28 (night) — M3 met on darksky.local: sensors → stored → store, D demo (Nick)

- **N2–N3 done** (`efc5261`): `src/sensors/i2c.{c,h}` (`I2C_RDWR`, repeated start), `chips.{c,h}`
  (per-chip setup/read), `daemon.{c,h}` (the shared loop: START, SAMPLE on absolute
  `CLOCK_MONOTONIC` deadlines, FAULT, exit 3 after 3 failed reads, `stored_down`/`stored_up`
  logging), `skyd`/`cloudd`/`envd` (one `main` each), `sensorcheck`. Linux-only; built by
  `make all` on Linux.
- **Evidence for M3 is in [`m3-checkpoint.md`](m3-checkpoint.md)**, all on live sensors:
  `sensorcheck` VALID ×3; a 70 s run of all three programs into `stored` (94 records = exactly
  what was sent, 0 dropped, no `seq` gaps); the D demo (`kill -9 stored` + a **provoked** 20-byte
  torn tail → `recover kept=28 cut_bytes=20`, sensors reconnected on their own, `cloudd`'s
  8 drops match its `seq` gap 20 → 29).
- **valgrind found a real bug** in `i2c.c`: uninitialized struct padding passed to
  `ioctl(I2C_RDWR)`. Fixed with `memset`. After the fix, `tools/valgrind-pipeline.sh` (all
  programs together on live sensors): 0 errors, all heap freed, for `stored`, `skyd`, `cloudd`,
  `envd`, `sensorcheck`.
- **Open for M4:** ≥ 20 real plug-pulls (evaluation #11), the `fsync` sweep (#10), `dskyd`
  (mechanism E), segments, batching. `tools/run-m3.sh` (N4) was not needed: the commands in
  `m3-checkpoint.md` start the pipeline; `dskyd` will replace any launcher at M4.

---

## 2026-10-28 (late) — Storage side S1–S5 done and pushed (Liam)

- **Commits:** `d6ddff7` (S1: `store_sync`, counters), `8f93e45` (S3 `stored`, S4 `store_client.c`,
  S5 `make itest`, plus the directory `fsync` Nick approved), `a4ac200` (S2: `store_scan`,
  `dsq dump`). S6 (write batching) not started; only if asked after M3 is safe.
- **Verified on topeka (CS department Linux, x86-64), not the Pi:** `make test` 496 checks,
  0 failed; `make itest` 137 checks, 0 failed (and 10 repeat runs); `make asan` both suites
  pass; `make clean && make memcheck` 0 errors, 0 bytes in use, for `test_all` and for
  `itest_stored` with `stored` traced. Tests were written first and seen failing each time.
- **Not yet verified:** any of it on `darksky.local` (step 1 of the integration run).

### Build targets (confirmed with Nick)

| Target | What | Where |
|---|---|---|
| `build/dsq` | `dsq dump` | `make all`, macOS and Linux |
| `build/stored` | the daemon | `make all`, Linux only |
| `build/itest_stored` | integration test | `make all`, Linux only |
| `make itest` | runs `./build/itest_stored ./build/stored` | Linux; prints "needs Linux" on macOS |

- `make asan` now also runs `itest` on Linux; `make memcheck` also runs it under valgrind with
  `--trace-children=yes`, so `stored` itself is checked.
- **`STORE_CLIENT_SRC`** (`src/store/store_client.c`) is the variable for the sensor programs'
  link lines, for example
  `$(CC) $(CFLAGS) src/sensors/skyd.c ... $(STORE_CLIENT_SRC) $(COMMON_SRC) -o build/skyd`.
  The client needs only `src/common/` (record, CRC, logger), not the store library.
- `dsq`'s formatting is in `DSQ_LIB` (`src/dsq/dump.c`), so the test binary and `build/dsq`
  share it; `stored` does not link it.
- Known, unchanged: after `make asan`, run `make clean` before `make memcheck` (both build into
  `build/`, so memcheck would otherwise run the ASan binaries).

### How to run it (for N4 / `tools/run-m3.sh`)

```sh
mkdir -p ~/dsm/data ~/dsm/run
./build/stored ~/dsm/data/seg-000001.dsm ~/dsm/run/stored.sock 2>> ~/dsm/run/stored.log &
# then the sensor programs, each given ~/dsm/run/stored.sock
./build/dsq dump ~/dsm/data/seg-000001.dsm -n 20
kill -TERM <stored pid>      # exit 0: drained, synced, socket removed
```

- `stored <data-file> <socket-path> [sync-seconds]`. The third argument is optional (default
  60, 1–3600); the itest uses 1. Exit **0** stopped by SIGTERM/SIGINT; **1** bad arguments, a
  file that isn't a store file, or the socket can't be set up (including a non-socket file at
  the socket path, which it refuses to delete); **2** a write or `fsync` failed.
- Log lines (stderr): `INFO start file=.. kept=.. sync_s=..`; `WARN recover kept=N
  cut_bytes=K` when recovery cut anything; `INFO sync records=.. bad=.. clients=.. fsyncs=..`
  every sync; `WARN bad_message len=..`, `WARN bad_record source=.. seq=..`; `INFO stop
  records=..`. Every start appends a START record from source 6 (`stored`).
- **Socket paths must be shorter than 108 bytes** (`sun_path`). `~/dsm/run/stored.sock` is fine.
- `store_client_send()` never blocks (`O_NONBLOCK`, `MSG_NOSIGNAL`): with `stored` down or
  behind it returns -1 and counts the drop; it retries the connection at most once per second.
  It logs only changes of state, under the sensor program's own name: `store_connect`,
  `store_connect_fail`, `store_disconnect`, `store_full`. Call `log_init()` before using it.
- On SIGTERM `stored` reads in whatever clients already sent (up to 1024 per client) before
  closing, so a clean stop loses no record whose `send()` succeeded.

### Note for the D demo write-up (`docs/m3-checkpoint.md`)

- **`kill -9 stored` leaves an expected `seq` gap.** Records already in the socket queue when it
  dies are lost, and `store_client_dropped()` does not count them: their `send()` succeeded. The
  next send after the kill fails (`EPIPE`) and is counted, and so is every send until the
  reconnect. So "no `seq` gaps" holds for the clean run (step 2), not across the kill (step 3).
  `itest` case "client through restart" shows the gap: seq 1–4 missing between two STARTs.
- **Directory `fsync`** runs on every `store_open()`, not only when the file is new: a crash
  between creating the file and syncing its directory would otherwise leave a file that looks
  old on the next start, with its name still not on the card.

### Open

- `stored`'s exit 2 (failed write or `fsync`) is covered only at the library level
  (`test_sync_on_broken_handle`); no itest case makes the daemon hit it.
- Nothing stops two `stored`s on the same data file; at M4 `dskyd` is the only launcher. An
  `fcntl` lock in `store_open()` would close it if wanted.
- `DESIGN.md` note on the M3 slice and SEQPACKET being Linux-only (Nick's close-out item).
- Cross-review: Liam's review of Nick's sensor code in `PROMPTLOG-liam.md`, once N3 is pushed.

---

## 2026-10-28 (evening) — Shared pieces + sensor conversion math (Nick)

- `1f74c77`: `record.h` (START kind, `enum dsm_source`, `enum dsm_fault`), `log.{c,h}`,
  `store_client.h` (header only; `store_client.c` is the storage side's).
- N1 done: `src/sensors/convert.{c,h}` + `tests/test_convert.c`. Every constant checked
  against the datasheet PDFs. MLX PEC tested on the datasheet's figures 6–8; MLX °C on its
  4.1.8.2 examples; TSL2591 full scale is **37888 at 100 ms** (not 65535); BME280 integer code
  cross-checked against Bosch's floating-point formulas. 389 checks, 0 failed; `make asan`
  clean; 8/8 mutants caught.
- `make asan` now uses `-fno-sanitize-recover=all`: undefined behavior fails the run instead of
  only printing. Proven: the datasheet's literal `<< 17` on a negative value passes the tests
  but now fails `make asan`.
- **Next on the sensor side:** N2 (I2C layer + `sensorcheck`, needs `darksky.local` on), then N3
  (`skyd`, `cloudd`, `envd`; these link only once `store_client.c` is pushed), N4, integration.

---

## 2026-10-28 — M3 started: crash recovery done; work split agreed (Nick)

- **Canvas "TP-D2" grades M2 and M3** (10 pts each; closes Fri 2026-10-30, 8 pm MT). M2 is met.
  M3 is not yet: nothing in the repo moves sensor data into storage.
- **Crash recovery is in** (`bc6d3bc`): `store_open()` on an existing file checks the header,
  then walks the records and cuts the file off at the first short or CRC-bad one (`ftruncate`,
  then `fsync` so the cut is durable). `store_get_recovery()` reports `kept` and `cut_bytes`.
  This closes "reopen appends after a partial record, out of line" from the 2026-10-27 entry.
  - Verified on macOS: `make test` 259 checks, 0 failed; `make asan` 259, 0 failed.
  - Six new tests: torn tail, corrupt middle record, zero-filled slot, clean file unchanged
    byte for byte, header-only file, recovery twice = once.
  - Mutation check: 5 of 5 deliberate breaks caught (no CRC check, off-by-one, never truncate,
    recovering a foreign file, wrong `cut_bytes`). Not provable by a unit test: that a failed
    `pread` cuts nothing, and that the `fsync` happens. The plug-pull test covers the latter.
- **Agreed on a call (Nick, Liam): the M3 work is split by side.** Liam: the storage side
  (`fsync`, counters, read-only scan, `stored`, `store_client.c`, `make itest`, `dsq dump`).
  Nick: the sensor side (conversion math, I2C layer, `skyd`, `cloudd`, `envd`), because only
  `darksky.local` is wired, and the integration run on it. `CLAUDE.md`'s ownership map is updated.
- **Decisions:** M3 slice = sensors → `stored` directly; `SOCK_SEQPACKET` kept as designed, which
  runs on Linux only (macOS: "Protocol not supported", checked); mechanism D is the one shown
  working at M3.
- **The M3 plan, the interface between the two sides, and each side's steps:
  [`m3-plan.md`](m3-plan.md). Read it before starting M3 work.**
- Makefile split into a storage section and a sensor section so each side edits only its own
  lines.
- **Open:** the shared pieces (`record.h` additions, `log.{c,h}`, `store_client.h`) land next,
  from Nick. Storage steps S1 and S2 don't need them.

---

## 2026-10-28 — Pulled Nick's bring-up; status check (Liam)

- Pulled `2deadc7`, `022d55d`, `b527bf2`. No code changed (`src/`, `tests/`, `Makefile`
  untouched). On a CS department Linux machine (x86-64), **not the Pi**: `make` exit 0;
  `make test` 116 checks, 0 failed; `make asan` 116 checks, 0 failed.
- **Closes from the 2026-10-27 entry:** "no sensor wired" / target 2026-11-03 (met 2026-10-28,
  hardware-log §1–2, `i2c-tools` checks, not project code); Nick's confirmation of items 1–11
  (the ‡ marks are gone); M2 transcripts copied by both; the `transcripts/README.md` folder name.
- **M3 state: unchanged.** The repo has the storage library (`src/common/`, `src/store/`) and its
  tests only. No sensor daemons, IPC, supervisor, `stored` daemon, recovery or `fsync`; no logs or
  soak output. Nothing in the repo shows M3 working. The spec estimate puts M3 at ~Oct 30;
  Canvas dates govern.
- **Open:** hardware-log §3–5 on `darksky.local`; `starlight.local` not wired; Nick's review of
  the `DESIGN.md` diffs; Liam's review of `src/store/` and `src/common/` in `PROMPTLOG-liam.md`;
  the BOM Decisions section (2 GB, 32 GB high-endurance) and `design-review.md` CHALLENGE 3
  (RTC "not optional") still contradict the design.
- **To agree with Nick and the TA:** `CLAUDE.md` says transcripts are copied as-is; Nick's M2
  copy redacts a password. Whether to write that exception into `CLAUDE.md` is not decided.

---

## 2026-10-28 — M2 confirmations, transcripts, submission (Nick)

- Pulled Liam's 11 commits. `make test` 116 checks, 0 failed (macOS).
- **Confirmed items 1–11 of the 2026-10-27 entry as written**, including the analysis split
  (Nick: features, ephemeris, `dsq`; Liam: classifier + state machine). The ‡ marks are gone
  from `DESIGN.md`; `README.md`, `CLAUDE.md` and `record-and-ipc.md` say the same. Nick's
  line-by-line review of the `DESIGN.md` diffs (his PROMPTLOG cross-review episode) is still open.
- **Transcripts:** `transcripts/nick/2026-10-28-m2/`, the term-project session through today
  plus its two subagent sessions. One change from as-is: a password typed into chat on
  2026-10-27 is replaced with `[REDACTED-PASSWORD]` (see `transcripts/README.md`). The session
  keeps going after this snapshot; the M3 copy picks up the rest.
- **Sensors wired and alive** on `darksky.local`, closing M2's "both sensors electrically
  alive" before submission. Raw output in `hardware-log.md` §1–2: all three answer at `0x29`,
  `0x5a`, `0x76`; the BME280 is genuine (`0x60`); the TSL2591 ID is `0x50`; first MLX90614 and
  TSL2591 readings are plausible. Wiring table in §1. Still open: hardware-log §3–5.
- **Submission files** (outside the repo): `Term Project/submissions/cs370-DarkSkyMonitor-M2-20261028.zip`
  and `.tar.gz`, built from a clean clone of the commit that adds this entry.

---

## 2026-10-27 (evening) — M2 design document written (Liam)

Nick was unavailable, so Liam wrote `DESIGN.md` alone for the M2 deadline (2026-10-28, Canvas
date). The course handout was read in full and is kept locally only (`docs/CS370-TermProject.pdf`,
git-ignored).

### What changed

| Commit | What |
|---|---|
| `f85b220` | `.gitignore` the handout PDF |
| `4a75922` | DESIGN: architecture diagram with a rate on every arrow |
| `4deb2f6` | DESIGN: mechanism mapping. E: R-E1 (UNKNOWN within 60 s), R-E2 (≤ 10 s lost per crash). D: R-D1 (≤ 60 s lost per power cut), R-D2 (20/20 recoveries), R-D3 (≥ 180 days on 16 GB) |
| `584ac8b` | DESIGN: failure-mode table (13 rows, each with how to inject it) and storage |
| `32a55e8` | New [`record-and-ipc.md`](record-and-ipc.md): record layout, payloads, rate derivation, ring invariants, channels, supervision rules |
| `9fa7f0b` | DESIGN: constraints and evaluation plan (14 rows, each with a numeric target) |
| `b053730` | DESIGN: ownership, AI-use plan, trimmed to 5 pages |
| this commit | `CLAUDE.md`, hardware log, this entry |

### How it was verified

- On a CS department Linux machine (x86-64, gcc 16.1.0), **not the Pi**: `make` exit 0;
  `make test` 116 checks, 0 failed; `make asan` 116 checks, 0 failed; `make clean && make
  memcheck` 0 errors, no leaks.
- `DESIGN.md` is 5 pages rendered by pandoc → PDF (xelatex, 11 pt, 1-inch margins, US letter).
  The handout sets no format; this is the measure we used.
- `grep -rnE 'AF_INET|PF_INET|getaddrinfo|gethostbyname|curl|https?://' src/` prints nothing.

### Decisions marked "proposed (Liam), Nick to confirm" (‡ in DESIGN.md)

1. **Hub → storage:** `AF_UNIX SOCK_SEQPACKET`, one record per message. `stored` accepts
   `fusiond` and `dskyd` and stays the only writer. While `stored` is down, `fusiond` buffers 4096
   records (≈ 50 min), then drops the oldest and counts.
2. **Durability:** `write()` every 10 s or 64 records; `fsync()` every 60 s, at rotation and at
   shutdown; directory `fsync()` after creating a segment.
3. **Segments:** one per night, rotated at local noon (every 24 h of monotonic time when the clock
   is unsynced), 32 MB cap. No MANIFEST: recovery scans only the open segment.
4. **Retention:** raw 180 days, rollups forever; raw writes stop below 2 GB free.
5. **Torn header:** a 1–23-byte file with our segment name in our data directory is recreated; any
   other short file is still refused.
6. **`seq` per (source, run):** a new kind START (= 6) opens every run, carrying the run number
   from `dskyd`.
7. **Failures in records:** an I/O failure is a FAULT (the first, then ≤ 1 per 60 s); an
   implausible value is a SAMPLE without `VALID`.
8. **`source_id`s:** 0 reserved, 1 `skyd`, 2 `cloudd`, 3 `envd`, 4 `fusiond`, 5 `dskyd`,
   6 `stored`.
9. **Payloads:** Nick's SAMPLE table adopted as is; FEATURE, STATE, HEARTBEAT, FAULT and START
   added; new flag `CLOCK_UNSYNC` = bit 4. No layout change. See `record-and-ipc.md`.
10. **Analysis split:** Liam takes the classifier + state machine; Nick takes features,
    ephemeris and `dsq`.
11. **Evaluation targets in Nick's area:** row 2 (durable p99 ≤ 61 s), 10 (`fsync` sweep, ≤ 30
    MB/day written at 60 s), 11 (power cut 20/20, ≤ 60 s lost), 12 (ephemeris ≤ 0.1° sun, ≤ 0.5°
    moon).

### Decided in Liam's area (mechanism E)

Three sensor daemons share the bus; an I2C broker is the fallback if p99 interval jitter > 100 ms.
Shared-memory single-producer/single-consumer rings, chosen for decoupled lifetimes. One thread
per process. A hang is detected after 20 s of a silent liveness pipe. A "sensor absent" exit code
→ `DEAD` + re-probe every 60 s. Crash backoff 1 → 60 s; 5 crashes in 300 s → `DEAD`. systemd
supervises `dskyd`; children use `PR_SET_PDEATHSIG`. Hourly heartbeat; stderr → journald. No IP
sockets. With no RTC: `CLOCK_UNSYNC` flag and an UNKNOWN verdict.

### Hardware

- **Two kits.** `darksky.local` (Nick) is the device of record: soak, demo and every evaluation
  number. `starlight.local` (Liam: Pi $45 + sensors $32 = $77) is for development only. Team
  ≈ $145 against the handout's $50–80 guideline, before the unrecorded PSUs, SD cards, RTC and
  enclosure.
- DS3231 RTC: to be ordered.
- **No sensor wired on either kit.** "Both sensors electrically alive" was not met for M2; target
  2026-11-03 for hardware-log §1–5 on `darksky.local`. *(Superseded 2026-10-28, see above.)*

### Open

- **Nick:** confirm or change items 1–11 above, then review the DESIGN.md diffs (that review is
  the PROMPTLOG cross-review episode).
- **Liam:** write up a review of Nick's `src/store/` and `src/common/` in `PROMPTLOG-liam.md`.
  Findings so far: a 1–23-byte file is refused forever (item 5 addresses it), no directory
  `fsync` after create, and `seq` resets on restart (item 6 addresses it).
- **Both:** copy out M2 transcripts to `transcripts/<name>/2026-10-27-m2/`.
- **Makefile bug, not fixed:** `make memcheck` straight after `make asan` runs valgrind on the
  ASan binary and crashes. Run `make clean` first. `vgcore.*` is also missing from `.gitignore`.
- **Docs that still contradict the design:**
  - README says the Pi isn't ordered and the project is at M0.
  - The BOM's Decisions section says Pi 4 2 GB, "buy Adafruit" and a 32 GB high-endurance card;
    we have 1 GB, clones and a standard 16 GB card. It also argues for both a narrow and a wide
    field of view.
  - `design-review.md` says the project can't work without an RTC.
  - `transcripts/README.md` names `liam/2026-09-23-m1/`; the folder is `m0-m1`.
- **Prices not recorded:** PSUs, SD cards, card reader (both kits).
- **Commits after this one:** 26 in total, Liam 9 (≈ 35 %).

---

## 2026-10-27 — Pi set up; storage writer done (Nick)

### Milestone position

Canvas dates govern. The spec's estimates (Week 1 = Aug 24) put **M2 at week 7 (~Oct 9)** and
**M3 at week 10 (~Oct 30)**.

| Milestone | Requirement (spec §6) | State |
|---|---|---|
| M0 | Team, hardware ordered, repo + `CLAUDE.md` | Done |
| M1 | Problem memo, transcripts | Done (`PROBLEM.md`; named-user gap recorded in its header) |
| **M2** | `DESIGN.md` (spec §8) | **Not written.** Still the section skeleton. Most of the material exists in `docs/design-review.md` |
| **M2** | Both sensors electrically alive | **Pi ready, sensors not wired yet.** Wiring and bring-up planned for the evening of 2026-10-27 |
| **M2** | Transcripts copied out | **Not done for M2** (only `transcripts/*/2026-09-23-m0-m1/` exist) |
| M3 | Both sensors through the real pipeline into real storage; one menu mechanism working | Storage writer exists (below); no sensor code, pipeline, or supervisor yet |

Commit count: **16** (Nick 15, Liam 1). The spec requires ≥ 40 meaningful commits across the
project with both partners well represented.

### Hardware

All parts arrived 2026-10-13. The Pi was set up headless on 2026-10-27. Full detail and raw output
are in [`hardware-log.md`](hardware-log.md) §0.

- Raspberry Pi OS Lite 64-bit. Hostname `darksky`, reached as `darksky.local` from the same Wi-Fi.
  Nick's Pi, Nick's login.
- Installed: `git build-essential i2c-tools valgrind rsync`. I2C is on (`/dev/i2c-1` exists).
- Power: `vcgencmd get_throttled` → `0x0`.
- First Linux build of the project: `make test` 15/0, `make memcheck` 0 errors, no leaks.
- **Not done yet:** sensor wiring, `i2cdetect`, BME280 chip ID, and the rest of the bring-up
  checklist in `hardware-log.md` §1–5.
- **No RTC** (DS3231 deferred). The Pi gets the real time from the network at boot. Without a
  network, `CLOCK_REALTIME` is wrong after a power cut, and so is anything that depends on it
  (`t_real_ns`, and later the sun/moon calculations).

### Code that exists

Everything below builds with `-std=c17 -Wall -Wextra -Werror` and passes `make test` and
`make asan` on macOS. Steps 0–2 also pass `make memcheck` on the Pi.

| File | What it is | Owner (CLAUDE.md map) |
|---|---|---|
| `Makefile` | One test binary `build/test_all`; targets `test`, `asan`, `memcheck`, `deploy`, `clean`, `help`. `-D_POSIX_C_SOURCE=200809L` is required for glibc to declare `fsync`, `pread`, `clock_gettime`, etc. under `-std=c17` | shared |
| `tests/check.h`, `tests/test_main.c`, `tests/suites.h` | `CHECK(cond, fmt, ...)` counts and prints failures; `main` runs every suite and exits non-zero on any failure | shared |
| `src/common/crc32.{c,h}` | `dsm_crc32(data, len)`: CRC-32/IEEE, bit-at-a-time. Tested against the published check value `0xCBF43926` | Nick |
| `src/common/record.{c,h}` | `struct dsm_record`: **the 48-byte format every process shares** (see the contract below) | Nick |
| `src/store/store.{c,h}` | Append-only store file: a 24-byte header, then records back to back | Nick |

Tests: 116 checks across `tests/test_crc32.c`, `tests/test_record.c`, `tests/test_store.c`.

### The record contract (`src/common/record.h`)

Everything that produces data produces `struct dsm_record`. The rules:

1. `record_init(&r, kind, source_id)` first. It zeroes all 48 bytes. Uninitialized bytes would be
   fed into the CRC, which valgrind reports.
2. Fill in the fields.
3. `record_seal(&r)` **last**. Any change after sealing makes `record_intact()` false.
4. `seq` counts per source, starting at 0, +1 per record. A gap is provable lost data.
5. `t_mono_ns` = `CLOCK_MONOTONIC` (for intervals); `t_real_ns` = `CLOCK_REALTIME` (wall-clock
   label).
6. Replayed or simulated data **must** set `DSM_FLAG_SYNTHETIC` (spec §5).
7. Values in `v[0..3]` are integers (fixed-point), never floats.

**The store refuses any record that isn't sealed** (`STORE_ERR_BADREC`). That's deliberate. Crash
recovery will cut a file off at its first bad record, so letting one unsealed record in would cost
every good record written after it.

### Proposed sample payload (NOT agreed — both partners to decide)

Which `source_id` each sensor uses, and what goes in each `v[]` slot, is a cross-boundary
interface. Nothing depends on it yet. This is only a starting proposal:

| `source_id` | Process / sensor | `v[0]` | `v[1]` | `v[2]` | `v[3]` |
|---|---|---|---|---|---|
| 0 | *(reserved: a zeroed record must never look real)* | | | | |
| 1 | `skyd` / TSL2591 | CH0 raw count (full spectrum) | CH1 raw count (IR) | gain multiplier (1, 25, 428, 9876) | integration time, ms |
| 2 | `cloudd` / MLX90614 | sky (object) temp, m°C | sensor (ambient) temp, m°C | 0 | 0 |
| 3 | `envd` / BME280 | temperature, m°C | humidity, milli-%RH | pressure, Pa | 0 |

The reasoning behind it:

- **Raw TSL2591 counts rather than lux.** Lux isn't meaningful at night-sky levels, and the
  design converts brightness to relative mag/arcsec² as a *feature* later (`design-review.md` §6).
  Storing raw counts plus gain and integration time means that conversion can be redone from old
  data.
- **The MLX90614 reports in steps of 0.02 K**, which is exactly 20 m°C, so milli-degrees store it
  with no rounding: `m°C = raw * 20 - 273150`.

Still open: how a failed read is recorded (a `FAULT` record, or a `SAMPLE` without
`DSM_FLAG_VALID`), and the `source_id`s for `fusiond` and the supervisor.

### Store API (`src/store/store.h`) as of today

```c
enum store_status store_open(const char *path, struct dsm_store **out);   /* create or reopen */
enum store_status store_append(struct dsm_store *s, const struct dsm_record *r);
enum store_status store_close(struct dsm_store *s);                         /* NULL-safe */
```

Status values: `STORE_OK`, `STORE_ERR_IO` (check `errno`), `STORE_ERR_FORMAT` (an existing file
that isn't ours; refused, left untouched), `STORE_ERR_BADREC` (unsealed record; nothing written),
`STORE_ERR_BROKEN` (an earlier write failed; no more appends on this handle).

What it guarantees **today**, and what it doesn't:

- ✅ New file gets a header: magic `DSKY`, schema version, record size, creation time, CRC.
  Reopening appends at the end and never rewrites the header.
- ✅ Wrong magic, schema, or record size, a damaged header, or a file shorter than a header →
  refused, and the file is left untouched.
- ✅ After a failed or partial write, the handle refuses all further appends, so nothing gets
  written out of line with the 48-byte slots. Tested by lowering `RLIMIT_FSIZE` to make a real
  `write()` fail.
- ✅ The fd is `O_CLOEXEC`, so it isn't inherited across `exec` by supervised children.
- ❌ **No `fsync` yet.** A power cut can lose records the kernel hasn't written to the card yet.
- ❌ **No recovery yet.** If a crash leaves a partial record at the end, reopening appends after
  it, out of line. The recovery step fixes this.
- ❌ One writer per file is assumed. The design gives storage a single writer process (`stored`).
- Open question for recovery: a file of 1–23 bytes is refused today. It could be a crash during
  header creation (ours) or a stranger's file, and there's no way to tell them apart.

**Remaining storage steps:** (4) recovery: scan, validate each CRC, truncate at the first bad
record, report what was discarded; (5) write batching + `fsync` policy, with counters for the
fsync experiment. After those: segment rotation, `MANIFEST`, nightly rollups, free-space guard,
the `dsq` query CLI.

### Building and running on the Pi

- From a Mac or Linux clone: `make deploy PI=<user>@<host>` (rsync; copies changed files, skips
  `.git`, `build`, `transcripts`), then on the Pi `cd ~/term-project && make test && make memcheck`.
- `valgrind` only runs on Linux. The Pi or a CS department machine is where `make memcheck` means
  something.
- **Linux-only code needs a Makefile split that doesn't exist yet.** The Makefile currently builds
  every file in `SRC` on every machine, and one partner develops on macOS. Code that includes
  `<linux/i2c-dev.h>` or calls `ioctl(I2C_RDWR)` would break the Mac build if it went straight
  into `SRC`. Keeping it in `src/sensors/` (per `CLAUDE.md`) and adding it through a Linux-only
  variable (e.g. `ifeq ($(shell uname -s),Linux)`) keeps `make test` working everywhere.

### Open work for M2 (none of it is started)

**1. `DESIGN.md`** (10 pts, team). Spec §8: architecture with rates on every arrow, mechanism
mapping justified from the user's requirements, failure-mode table, storage and data,
constraints and substitutions, evaluation plan, ownership map, AI-use plan. Sources:
`design-review.md` §5 (architecture), §8–9 (data, storage), §10–11 (processes, IPC), §12
(failure modes), §13 (evaluation).

**2. Sensors electrically alive.** Evidence goes in `hardware-log.md` §1–5: `i2cdetect -y 1`
should show `0x29`, `0x5A`, `0x76`/`0x77`. BME280 chip ID first, since it has a return window.

**3. Sensor-reading code, `src/sensors/`** (the CLAUDE.md ownership map lists this area under
mechanism E). Nothing exists. A natural split that can be built and tested without hardware:

- **Pure conversion functions:** register bytes in, fixed-point units out (the proposed payload
  above). No I/O, so they build anywhere. Unit-test them with the worked examples in the
  datasheets.
- **A thin Linux I2C layer:** `open("/dev/i2c-1")` + `ioctl(I2C_RDWR)` with a combined
  write-then-read (repeated start). Linux-only, see the Makefile note above.
- **A one-shot check program** that reads all three sensors once and prints values. This is the
  M2 "electrically alive" evidence, and it becomes the core of `skyd` / `cloudd` / `envd` at M3.

**Register facts to start from. Verify every one against the datasheet PDF before relying on
it. The team rule is evidence over recall, and these came from recall.**

| Sensor | Address | Identity check | Key registers / protocol | Gotchas |
|---|---|---|---|---|
| BME280 | `0x76` (or `0x77`) | `0xD0` → `0x60`. `0x58` means a BMP280 (no humidity) | Calibration `0x88–0xA1` and `0xE1–0xE7`; `ctrl_hum 0xF2`, `ctrl_meas 0xF4`, `config 0xF5`; data burst `0xF7–0xFE` (P, T, H). Forced mode: write `ctrl_meas` with mode `01`, wait, read | Write `ctrl_hum` **before** `ctrl_meas` or the humidity setting is ignored. Use the datasheet's integer compensation code (32-bit T and H, 64-bit P); temperature must be computed first because P and H use its `t_fine` |
| TSL2591 | `0x29` | ID register `0x12` → `0x50` | Every register access goes through the command byte `0xA0 \| reg`. `ENABLE 0x00` (PON + AEN = `0x03`), `CONTROL 0x01` (gain bits 5:4, integration time bits 2:0: 100–600 ms), `STATUS 0x13` (AVALID bit 0), CH0 `0x14–0x15`, CH1 `0x16–0x17`, little-endian | At max gain indoors it saturates. The dark offset (hardware-log §5) is the noise floor every night reading sits on |
| MLX90614 | `0x5A` | — | SMBus read-word from RAM: `0x06` = ambient, `0x07` = object. Returns 3 bytes: LSB, MSB, PEC. `°C = raw × 0.02 − 273.15` | Needs a **repeated start** (combined write-read). PEC is CRC-8 (poly `0x07`) over addr+W, command, addr+R, LSB, MSB: check it. MSB bit 7 set on an object reading = error flag. Known Pi problem with this chip's clock stretching: lower `i2c_arm_baudrate` or use `i2c-gpio` (design-review §3) |

**4. Supervisor + IPC (mechanism E)** — not needed for M2, needed for M3. `design-review.md`
§10–11: `dskyd` fork/execs the children, sees each death through pipe EOF / `SIGCHLD`, restarts with
exponential backoff, and gives up with a logged `DEAD` state on a restart storm. Can be built and
tested on any Linux machine with stand-in child processes; no sensors needed.

**5. Process items for M2.** Each partner copies out their `.jsonl` transcripts to
`transcripts/<name>/<date>-m2/`. Each partner's `PROMPTLOG` needs, at some point, a review of
the *other* partner's code. `src/store/` and `src/common/` are reviewable now. The ownership map's
"proposed" side still needs confirming in `CLAUDE.md`.
