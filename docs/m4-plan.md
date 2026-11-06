# M4 plan: from "records data" to "judges the night", then the soak

**Status: written by Nick, 2026-11-06. Liam hasn't agreed to it yet.** Sections marked
**proposed** are open to change by agreement. Progress goes in
[`progress-log.md`](progress-log.md), newest entry first.

## What M4 requires

Spec §6: **feature freeze; 48-hour soak begins; transcripts copied out**, in week 14 (Nov 23–27; it
may shift for fall break, so check Canvas). The spec says to leave room to run the soak twice, so
the **target for freezing the code is Nov 20**.

Today the device records data, but it has no supervisor, no analysis and no verdict.

## Who does what (matches the ownership map in `CLAUDE.md`)

| Area | Files | Who |
|---|---|---|
| Shared interfaces (below) | `src/analysis/{ephemeris,features,verdict}.h`, `record.h` flag | Nick wrote them; change by agreement |
| Sun and moon position | `src/analysis/ephemeris.c`, `tests/test_ephemeris.c` | Nick |
| Features | `src/analysis/features.c`, `tests/test_features.c` | Nick |
| Synthetic scenes, `dsq` query commands | `mkscene`, `src/dsq/` except `dump.c` | Nick |
| Sensor daemons ready for `dskyd`, fake sensor | `src/sensors/`, `tests/itest_sensors.c` | Nick |
| Rings | `src/ipc/ring.{c,h}` | Liam |
| `stored`: segments, batching, free-space guard | `src/store/` | Liam |
| `dsq dump` | `src/dsq/dump.c` | Liam |
| Supervisor | `src/super/` (`dskyd`) | Liam |
| Hub | `src/fusiond/` | Liam |
| Classifier + state machine | `src/analysis/classify.c`, state machine | Liam |
| Site baseline | **no owner yet** (question 2 below) | ? |
| Soak, demo, every evaluation number | run on `darksky.local` only | Nick runs them, with both sides' code |

Files you both touch: the `Makefile` (each edits **only their own section**), `tests/suites.h` and
`tests/test_main.c` (one line per new suite), `docs/progress-log.md`. Same rule as M3: `git pull
--rebase` before every commit, small commits, never reformat the other side's lines.

**Commit steadily.** HW2's transcript score was docked for "74% of activity in the final days", and
the spec wants ≥ 40 meaningful commits with both partners well represented.

## Contracts between the two sides

### Analysis library (written, in `src/analysis/`)

The headers are the contract; read their comments. All of it is portable, so it builds and tests on
a Mac. The numbers on disk are pinned by `tests/test_verdict.c`.

- `features_new`, `features_push`, `features_get`, `features_free` (Nick). No clock inside: the
  caller passes the time.
- `ephem_compute` (Nick). `EPHEM_SITE_FORT_COLLINS` is a rounded city-center location.
- `classify` (Liam). Pure: the verdict for one moment. It has no memory of earlier moments, so dwell
  time and hysteresis go in the state machine, which takes these verdicts as input.

### What `fusiond` does with it (proposed)

1. Every record drained from a ring: `features_push`, then forward it to `stored`.
2. **Every 150 s:** `features_get` + `ephem_compute`; write the 10 features as FEATURE records,
   skipping any whose `ok` is false. 10 per 150 s = **4 per minute, the rate already in
   `record-and-ipc.md`**, so the storage numbers in `DESIGN.md` still hold. Faster would need them
   recomputed.
3. `classify`, then the state machine; write STATE on change and at least every 10 minutes.
4. **`synthetic` in means `DSM_FLAG_SYNTHETIC` out**, on every FEATURE and STATE record.
5. Clock unsynced: `clock_synced = false`, `DSM_FLAG_CLOCK_UNSYNC` on the records.

Payloads: [`record-and-ipc.md`](record-and-ipc.md) § Payloads.

### Sensor daemon ↔ `dskyd` (proposed)

Nick's `daemon.c` provides this; Liam's `dskyd` relies on it.

- **Started as** `skyd <output> <run> <liveness-fd>` (same for `cloudd`, `envd`). `<output>` is the
  `stored` socket today and the ring name once rings land. `<run>` goes in the START record.
- **Exit codes:** 0 stopped by SIGTERM/SIGINT; 1 bad arguments or setup failure; **3 sensor absent**
  (3 failed reads in a row, or the chip didn't answer at startup) → `dskyd` parks it as `DEAD` and
  re-probes every 60 s rather than restarting fast.
- **Liveness:** one byte down `<liveness-fd>` at least every 5 s. The loop wakes up for this between
  samples, since `envd` only samples every 10 s. A hung I2C read stops the bytes, which is the point.
- **`PR_SET_PDEATHSIG`:** set by the child, so children exit if `dskyd` dies.
- **Logs:** stderr, in the `log.h` format.

### Ring API that the daemons call (Liam designs; proposed minimum)

The daemons need: open an existing ring by name, push one sealed record without ever blocking
(returns 0 or "dropped"), and read the overrun count. `dskyd` creates the rings before the fork. Nick
keeps sending through `store_client` until `ring.h` lands, then swaps that one call in `daemon.c`.

### `dsq` ↔ `fusiond` socket (proposed)

`AF_UNIX SOCK_STREAM`, `~/dsm/run/fusiond.sock`. The client sends one line; the server replies with
`k=v` lines, then a line `end`, then closes.

```
state    → state=GOOD rules=0x0 confidence=0.870 since_ns=<t_real_ns> synthetic=0
           end
health   → source=skyd seen=1 stale=0 completeness=0.980 accepted=1234 rejected=0
           source=cloudd ...
           source=envd ...
           end
```

If `connect()` fails, `dsq` prints `state: UNKNOWN (hub down)` and exits 2 (`DESIGN.md` failure table).

### Segment files

`<dir>/seg-NNNNNN.dsm`, numbered upward. `dsq` reads them in number order, read-only. **No
`MANIFEST`:** `DESIGN.md` doesn't have one, and scanning the open segment is the whole recovery.
(`m3-plan.md` and `m3-checkpoint.md` mention one; that was the older design.)

### Fake sensor, for testing with no hardware

`build/fakesensor <sky|cloud|env> <output> <run> <liveness-fd>` behaves like a real daemon, but its
reads return scripted values and every record is `SYNTHETIC`. It can also fail on command, so
`dskyd`'s absent, restart and storm paths can be tested on any Linux machine. **The soak and the demo
never use it** (spec §3.4).

## Questions for Liam

1. **Does an immature baseline block POOR?** The design review checks UNKNOWN (including "baseline
   immature") before the POOR veto, so with no baseline the device could never say "overcast, POOR",
   even though no baseline is needed to see that. Suggestion: the baseline only gates GOOD.
2. **Who owns the site baseline?** `DESIGN.md` gives rollups to Nick; `CLAUDE.md` gives `src/store` to
   Liam. **Rule 6 only allows GOOD against a baseline, so without one the device can never say GOOD.**
   Suggestion: Nick, as `dsq baseline`, since it's built from features.
3. **Are `DEGRADED` and `FAULT` states needed?** The design review has them; `DESIGN.md`'s failure
   table only ever uses UNKNOWN, so `verdict.h` leaves them out. They can be added at the end if wanted.
4. **`dsq dump` output (`dump.c`, yours):** `CLOCK_UNSYNC` now exists, but `dump.c` prints it as
   `0x10`. Add it by name, and consider printing state and feature names instead of numbers.

## Steps

Same rhythm as M3: tests first and failing for the right reason, then code, then `make test` + `make
asan` green (plus `make itest` and valgrind on Linux), then commit `M4: <what>`.

**Nick** (none of these wait on Liam):

- **N1. Ephemeris.** Checked at ≥ 50 dates against JPL Horizons (`DESIGN.md` #12: ≤ 0.1° sun, ≤ 0.5° moon).
- **N2. Features.** Fixed memory: one summary per minute, 180 minutes kept.
- **N3. Synthetic scenes + `dsq analyze`.** Real store files, every record `SYNTHETIC`. They exercise
  code paths and are **never evaluation evidence**.
- **N4. `dsq` queries:** `range`, `gaps`, `heartbeats`, `why`, `state`, `health`.
- **N5. Daemons ready for `dskyd` + fake sensor + `itest_sensors`,** on CS Linux machines.
- **N6. Site baseline,** if question 2 lands with Nick.

**Liam** (his to order; suggested):

- **L1. Rings,** with the five invariants in `record-and-ipc.md`, including a producer killed mid-write.
- **L2. `dskyd`:** fork/exec, liveness, backoff, `DEAD` + re-probe, storm limit, heartbeats, systemd
  unit. Can use the fake sensor once N5 lands.
- **L3. Classifier + state machine.** Veto rules, dwell time, asymmetric hysteresis, published
  confidence formula. Portable, so it can start now against `verdict.h` and synthetic scenes.
- **L4. `fusiond`:** the loop above, plus the `dsq` socket.
- **L5. `stored`:** segments and rotation, torn-header recreation, batching (S6, still open from M3),
  free-space guard. Also open from M3: `stored`'s exit 2 has no itest, and nothing stops two `stored`s
  on one file.
- **L6. `make soakcheck`** made real. It's a placeholder today, and `CLAUDE.md` treats a lying target as
  a bug.

## Needs the Pi (`darksky.local`)

Backend tests prove the code runs, not that it's right about the sky. These can't be replaced:

1. **Most urgent: record outside overnight + keep the observation log.** The M3 pipeline can already
   record. `DESIGN.md` needs ≥ 40 cloud observations and ≥ 20 night verdicts; these can't be crammed,
   and the data can go through the classifier later (labeled as replay).
2. Hardware-log §3–5, especially whether the window material blocks the IR.
3. Outdoor brightness test: 25x gain / 100 ms is probably too weak for a night sky (expected, not measured).
4. Humidity fit, real thresholds, real baseline nights.
5. ≥ 20 real plug-pulls (#11), `fsync` sweep (#10), latency (#1), isolation cost (#9), valgrind on the Pi.
6. The soak, twice (#14). Hourly heartbeat, ≥ 1 unplug, ≥ 1 `kill -9`.

## Close-out

- `progress-log.md` entry; `DESIGN.md` changelog; correct the (a) values with measurements.
- **Transcripts:** each partner copies their own sessions into `transcripts/<name>/<date>-m4/`.
- **Each partner's `PROMPTLOG` needs a review of the other's work.**
- Fresh GitHub clone → zip → extract → `make test`.
