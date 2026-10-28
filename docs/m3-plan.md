# M3 plan: both sensors through the real pipeline into real storage

Agreed by Nick and Liam on a call, 2026-10-28. **Canvas (TP-D2) closes Fri 2026-10-30, 8 pm MT.**
Progress against this plan goes in [`progress-log.md`](progress-log.md), newest entry first.

## What M3 requires

Spec §6, and row 2 of the Canvas TP-D2 rubric: *"Checkpoint demo: both sensors flowing through the
real pipeline into real storage; at least one menu mechanism working; transcripts copied out."*

## Decisions

- **M3 slice: sensor programs → `stored` → store file**, over `AF_UNIX` `SOCK_SEQPACKET`. The
  shared-memory rings, `fusiond` and `dskyd` come at M4. `DESIGN.md`'s architecture is unchanged;
  M3 builds its two ends and connects them directly.
- **`SOCK_SEQPACKET`, as designed. It runs on Linux only:** on macOS `socket()` fails with
  "Protocol not supported" (checked 2026-10-28). So `stored`, its client and their tests build and
  run only on Linux (the Pi, the CS machines). The portable code (record, CRC, logger, store
  library, conversion math, `dsq`) still builds and tests on a Mac.
- **Mechanism shown working at M3: D** (crash recovery + `fsync`). E comes at M4.
- **"Both sensors" = TSL2591 (`skyd`) + MLX90614 (`cloudd`).** `envd` (BME280) as well if time.
- **Runtime files live outside the deployed tree**, so `make deploy` never touches them:
  `~/dsm/data/seg-000001.dsm` and `~/dsm/run/stored.sock`. One segment for M3; rotation is M4.

## Who does what (no file is edited by both)

| Area | Files | Who |
|---|---|---|
| Done: CRC, record, store writer, **crash recovery** (`bc6d3bc`) | `src/common/crc32.*`, `src/common/record.*`, `src/store/store.*` | Nick |
| **Shared pieces**, written first (2026-10-28 evening), then changed only by agreement | `src/common/record.h` additions, `src/common/log.{c,h}`, `src/store/store_client.h` | Nick |
| **Storage side** (S1–S6 below) | `src/store/` (`store.c` additions, `stored.c`, `store_client.c`), `src/dsq/`, `tests/test_store.c`, `tests/test_dsq.c`, `tests/itest_stored.c` | Liam |
| **Sensor side** (N1–N4 below) | `src/sensors/` (conversion math, I2C layer, `sensorcheck`, `skyd`, `cloudd`, `envd`), `tests/test_convert.c`, `tools/run-m3.sh` | Nick |
| **Integration + the D demo** on `darksky.local`, the only wired kit | `docs/m3-checkpoint.md` | Nick runs it, with Liam's pushed code |

Files both of you touch: the `Makefile` (each edits **only their own section**), `tests/suites.h`
and `tests/test_main.c` (one line each per new suite), `docs/progress-log.md`. Rule: `git pull
--rebase` before every commit, small commits, never reformat the other side's lines.

## The contract between the two halves

### The socket

- `AF_UNIX`, `SOCK_SEQPACKET`. Path given on `stored`'s command line.
- **One message = one sealed `struct dsm_record`, exactly 48 bytes.** Nothing else is ever sent.
- Clients only send. `stored` never replies.
- Many clients at once: one per sensor program now, `fusiond` and `dskyd` at M4.
- `stored` is the only process that writes the data file, and the only one that ever truncates it.
- Why SEQPACKET rather than a stream: message boundaries are kept, so one bad message never
  misaligns the ones after it, and the connection can stay open.

### The client API: `src/store/store_client.h`

Nick writes this header with the shared pieces. Liam implements `src/store/store_client.c`.

```c
struct store_client;

/* Remembers the socket path. Does not connect yet, so it works whether
 * or not stored is running. NULL only if out of memory or the path is
 * too long for a sockaddr_un (errno says which). */
struct store_client *store_client_new(const char *socket_path);

/* Sends one sealed record as one message. Never blocks: if stored is
 * down, slow or gone, the record is dropped and counted, and the caller
 * keeps sampling (the seq gap shows the loss). Reconnects on its own,
 * at most one attempt per second. Returns 0 if sent, -1 if dropped. */
int store_client_send(struct store_client *c, const struct dsm_record *r);

/* Records dropped since store_client_new(). */
uint64_t store_client_dropped(const struct store_client *c);

/* Closes the connection and frees. NULL-safe. */
void store_client_free(struct store_client *c);
```

Behavior notes for the implementation:
- Connect lazily: on a send with no connection, and at most once per second (`CLOCK_MONOTONIC`).
- Send with `MSG_DONTWAIT | MSG_NOSIGNAL`. `EINTR` → retry. `EAGAIN` (stored is slow) → drop and
  stay connected. `EPIPE`, `ECONNRESET`, `ENOTCONN`, `ECONNREFUSED`, `ENOENT` → close, drop,
  reconnect later.
- **A sensor program must never stall because `stored` is slow or dead.** That is the point of
  separate processes (DESIGN.md, R-E1).

### What the sensor programs send

- At start: one **START** record, `seq` 0, `v[0]` = run number (0 at M3; `dskyd` assigns it at
  M4).
- Then one **SAMPLE** per period, `seq` +1 each: `skyd` every 5 s, `cloudd` every 1 s, `envd`
  every 10 s. About 1.3 records/s in total.
- A failed read: one **FAULT** (then at most one per 60 s while it lasts). Three failed reads in a
  row: the program exits with code **3** ("sensor absent", for `dskyd` at M4).
- Payloads: the table in [`record-and-ipc.md`](record-and-ipc.md) § Payloads.
- An implausible reading (saturated, MLX error bit) is a SAMPLE **without** `DSM_FLAG_VALID`.

### Shared additions (Nick, 2026-10-28 evening)

- `record.h`: `DSM_KIND_START = 6`; `enum dsm_source` = 1 `skyd`, 2 `cloudd`, 3 `envd`,
  4 `fusiond`, 5 `dskyd`, 6 `stored`; `enum dsm_fault` = 1 read failed, 2 wrong chip ID.
- `log.{c,h}`: `log_init(proc, stderr)` once (the second argument lets tests capture lines),
  then `log_msg(level, event, fmt, ...)` writes one line in the DESIGN.md format `<ISO time> <mono s> <proc>[pid] LEVEL event k=v`, for example
  `2026-10-28T19:02:11-0600 1234.567 stored[812] WARN recover kept=118 cut_bytes=31`.
  Levels: `LVL_INFO`, `LVL_WARN`, `LVL_ERROR` (not `LOG_*`, which `<syslog.h>` already uses).

## Storage side, step by step (Liam)

Same rhythm as the store work so far: tests first and failing for the right reason, then code,
then `make test` + `make asan` green (plus `make itest` and valgrind on Linux), then commit
`M3: <what>`. **S1 and S2 need nothing from the shared pieces and can start now.**

**S1. `fsync` + counters** (`store.c`, `store.h`, `tests/test_store.c`)
- `enum store_status store_sync(struct dsm_store *s)`: `fsync`. BROKEN if the handle is broken.
  If `fsync` fails, mark the handle broken and return IO (after a failed `fsync` we don't know
  what reached the card; `stored` exits and recovery runs on restart).
- `store_close()` syncs before closing. It still closes and frees if the sync fails, and returns
  IO.
- `struct store_counters { uint64_t records, writes, fsyncs; }` and
  `store_get_counters(const struct dsm_store *)`.
- **The library has no clock.** *When* to sync (every 60 s) is `stored`'s decision, so the
  library stays testable without timing.
- Tests: counters move as expected; `store_sync` on a broken handle returns BROKEN.

**S2. Read-only scan + `dsq dump`** (`store.c`, `store.h`, `src/dsq/`, `tests/test_dsq.c`)
- A public read-only scan:
  `enum store_status store_scan(const char *path, store_visit_fn fn, void *ctx, struct store_recovery *out)`
  with `typedef void (*store_visit_fn)(const struct dsm_record *r, void *ctx)`. Opens `O_RDONLY`,
  checks the header, calls `fn` on each intact record in order, stops at the first short or bad
  one, and reports `kept` and the tail bytes it stopped before. **It never changes the file**, so
  it is safe while `stored` is writing. Share the record walk with `recover()` in `store.c`; only
  `recover()` truncates.
- Test: a scan of a file with a torn tail reports the same counts recovery would, and the file
  stays byte for byte the same.
- `dsq dump <file> [-n N]` (`src/dsq/`; portable): the header, then one line per record: local
  time from `t_real_ns`, source name, kind, flags (show `SYNTHETIC` clearly), `seq`, and values
  in units from the payload table. Last line: `N records; torn tail: K bytes (not modified)`.
  Keep the decode/format function separate from `main` so `tests/test_dsq.c` can test it.
- `dsq`'s later query commands stay with Nick's analysis work; `dump` is part of M3 storage.

**S3. `stored`** (`src/store/stored.c`; Linux build only)
- `stored <data-file> <socket-path>`. Wrong arguments → usage, exit 1.
- `store_open` (recovery runs). FORMAT or IO → ERROR line, exit 1. If anything was cut:
  `WARN stored recover kept=N cut_bytes=K`. Always: `INFO stored start file=<path> kept=N`.
- Append its own START record (source `stored`, `seq` 0), so every restart is visible in the file.
- Socket: `unlink` the path first (a stale one is left behind by `kill -9`), `bind`, `listen`.
  Failure → exit 1.
- Signals: SIGTERM and SIGINT through `sigaction` **without** `SA_RESTART`; the handler only sets
  a `volatile sig_atomic_t` flag. Ignore SIGPIPE.
- Loop: `poll()` over the listener and up to 16 clients. Timeout = time until the next `fsync`.
  New connection → `accept` (and set `FD_CLOEXEC`). Client readable → `recv` into a 64-byte
  buffer with `MSG_TRUNC` (Linux then returns the real message length):
  - 0 → the client closed; close its fd.
  - exactly 48 → `store_append`. OK: count it. BADREC: `WARN stored bad_record source=.. seq=..`,
    drop it, keep the client. IO or BROKEN: `ERROR stored write_fail errno=..`, **exit 2**
    (DESIGN.md: exit, recover on restart).
  - any other length → `WARN stored bad_message len=..`, drop it, keep the client.
- Every 60 s (`CLOCK_MONOTONIC` deadline): `store_sync`, then
  `INFO stored sync records=N bad=M clients=C fsyncs=F`. A sync failure → ERROR, exit 2.
- On SIGTERM/SIGINT: `store_close` (syncs), `unlink` the socket, `INFO stored stop records=N`,
  exit 0.
- `-D_POSIX_C_SOURCE=200809L` hides GNU extras such as `accept4`. If a name you need is hidden,
  define `_DEFAULT_SOURCE` at the top of that one `.c` file, before any `#include`, instead of
  changing the global `CFLAGS`.

**S4. `store_client.c`** (Linux build only): implements the header above.

**S5. `make itest` + `tests/itest_stored.c`** (Linux only; on macOS the target prints that it needs
Linux, like `make memcheck` does for valgrind). Fork/exec `build/stored` on a temporary file and
socket, then:
1. Two clients send 50 records → SIGTERM → exit 0 → the file holds the header, `stored`'s START
   and the 50 records, intact and in order.
2. A 30-byte message, then a good record → the good record is stored and `stored` keeps running.
3. An unsealed record → refused and not in the file.
4. `kill -9` stored, append 20 junk bytes to the file (a provoked torn tail), restart → stderr
   shows `recover ... cut_bytes=20`, and the file is intact.
5. A client keeps sending through a `stored` restart: sends while it is down return -1 and count
   as dropped; after the restart they succeed.
6. A file that isn't ours → exit 1, and the file is untouched.
Run the itest binary under valgrind on Linux as well.

**S6. If time remains: write batching.** `store_set_batch(s, k)` (default 1, today's
write-through, so the existing tests stay valid); `stored` uses 64 records / 10 s. Tests: K
appends cost one `write()`; nothing reaches the file before a flush; close flushes; a failed
flush marks the handle broken.

Order that keeps integration unblocked: S1 → S3 + S4 + S5 → S2 → S6. **Suggested target: S1–S5
pushed and green on Linux by Thursday night**, so integration on `darksky.local` can happen
Friday.

## Sensor side, step by step (Nick)

- **N1. Conversion math** (`src/sensors/convert.{c,h}`, portable, Mac tests): MLX90614 `m°C =
  raw × 20 − 273150`, error bit, PEC (CRC-8, poly `0x07`); TSL2591 little-endian channel
  assembly and saturation; BME280 integer compensation (`t_fine` first). Constants checked
  against the datasheet PDFs.
- **N2. I2C layer + `sensorcheck`** (Linux): `/dev/i2c-1`, `ioctl(I2C_RDWR)` with a combined
  write-then-read (repeated start, required by the MLX90614). `sensorcheck` reads every sensor
  once and prints the values, with no socket, so the hardware is proven before `stored` exists.
- **N3. `skyd`, `cloudd`, `envd`** (Linux): chip ID check, START, SAMPLE on absolute
  `CLOCK_MONOTONIC` deadlines, FAULT and exit 3 as above, sending through `store_client`.
- **N4. `tools/run-m3.sh`**: starts `stored`, then the sensor programs, logs to files. A launcher
  only: no graded mechanism lives in it.

## Integration and the D demo (Friday at the latest, on `darksky.local`)

1. `make deploy`, then on the Pi: `make test`, `make memcheck`, `make itest`.
2. Run the pipeline for ≥ 10 minutes. `dsq dump` shows both sensors' records with real values and
   no `seq` gaps.
3. D demo: `kill -9 stored` mid-run → the sensor programs keep sampling and reconnect; restart →
   recovery logged. A provoked torn tail (junk bytes appended, **labeled as provoked**) →
   `cut_bytes` matches. One real plug-pull → recovery at boot.
4. Raw output (commands, logs, `dsq` output) into `docs/m3-checkpoint.md`.

## Close-out

- `progress-log.md` entry; `DESIGN.md` note on the M3 slice and SEQPACKET being Linux-only.
- **Transcripts:** each partner copies their own M3 sessions into
  `transcripts/<name>/<date>-m3/` (see `transcripts/README.md`).
- **Cross-review:** each partner's `PROMPTLOG.md` needs a review of the other's work; the M3 code
  on the other side of the socket is the natural one.
- Fresh GitHub clone → zip → extract → `make test` → upload to Canvas.

## Deferred to M4

Segments and rotation, `MANIFEST`, torn-header (1–23 B) recreation, `dskyd`, the shm rings,
`fusiond`, heartbeats, `CLOCK_UNSYNC`. If Friday is tight, cut in this order: batching → `envd` →
`dsq` polish → the plug-pull (`kill -9` plus the provoked torn tail still show D working).
