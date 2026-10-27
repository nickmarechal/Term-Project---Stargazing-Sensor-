# Progress log

Where the project stands, written so that either partner, or a fresh Claude Code session, can pick
up cold. **Read this after `CLAUDE.md` and before starting work.** Newest entry first. Facts and
evidence only; plans live in `DESIGN.md` and `docs/design-review.md`.

When you finish a chunk of work, add an entry at the top: date, who, what changed, how it was
verified, and what it leaves open.

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
