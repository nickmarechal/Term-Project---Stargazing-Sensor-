# M3 checkpoint: evidence

All runs on **`darksky.local`** (Pi 4, 1 GB, Raspberry Pi OS Lite 64-bit), 2026-10-28, live
sensors, indoors (so the "sky" reading is the room). Commands are exactly what was run; output is
pasted raw except where marked `...`. Sections 1–4 ran at `efc5261`; section 5 ran after the `i2c.c`
padding fix committed with this document.

M3 requires: **both sensors flowing through the real pipeline into real storage; at least one
menu mechanism working; transcripts copied out.** The M3 slice is sensor programs → `stored` over
`AF_UNIX SOCK_SEQPACKET` → store file ([`m3-plan.md`](m3-plan.md)). Mechanism shown: **D**.

## 1. Build and tests on the Pi

`make clean && make all` builds every program with `-std=c17 -Wall -Wextra -Werror` and no
warnings, including the Linux-only ones (`stored`, `skyd`, `cloudd`, `envd`, `sensorcheck`).

```
$ make test
496 checks, 0 failed
$ make itest
itest_stored: 137 checks, 0 failed
```

## 2. Hardware path with our own code: `sensorcheck`

Reads each chip once through `src/sensors/i2c.c` + `chips.c` + `convert.c`, no socket.

```
$ ./build/sensorcheck
skyd    v = 936 228 25 100  VALID          # TSL2591: CH0, CH1 counts, gain 25x, 100 ms
cloudd  v = 18750 18650 0 0  VALID         # MLX90614: sky 18.750 °C, sensor 18.650 °C
envd    v = 18560 40176 84685 0  VALID     # BME280: 18.560 °C, 40.176 %RH, 84685 Pa
exit=0
```

84685 Pa (846.85 hPa) is consistent with Fort Collins' elevation (~1500 m).

## 3. All three sensors through the pipeline into the store (70 s)

```
$ ./build/stored ~/dsm/data/seg-000001.dsm ~/dsm/run/stored.sock &
$ for p in skyd cloudd envd; do ./build/$p ~/dsm/run/stored.sock & done
$ sleep 70; pkill -TERM ...           # sensors first, then stored
```

`stored.log`:
```
2026-10-28T16:42:26-0600 62.607 stored[1453] INFO start file=/home/nickmarechal/dsm/data/seg-000001.dsm kept=0 sync_s=60
2026-10-28T16:42:27-0600 63.609 stored[1453] INFO client_new clients=1
2026-10-28T16:42:27-0600 63.609 stored[1453] INFO client_new clients=2
2026-10-28T16:42:27-0600 63.609 stored[1453] INFO client_new clients=3
2026-10-28T16:43:26-0600 122.614 stored[1453] INFO sync records=78 bad=0 clients=3 fsyncs=1
2026-10-28T16:43:37-0600 133.693 stored[1453] INFO client_gone clients=2
2026-10-28T16:43:37-0600 133.733 stored[1453] INFO client_gone clients=1
2026-10-28T16:43:37-0600 133.757 stored[1453] INFO client_gone clients=0
2026-10-28T16:43:38-0600 134.765 stored[1453] INFO stop records=94
```

Sensor programs: `skyd ... stop records=14 dropped=0`, `cloudd ... stop records=71 dropped=0`,
`envd ... stop records=8 dropped=0`.

`./build/dsq dump ~/dsm/data/seg-000001.dsm`:
```
header: schema=1 record_size=48 created=2026-10-28T16:42:26.753-0600
2026-10-28T16:42:26.760-0600 stored  START     seq=0 flags=- run=0
2026-10-28T16:42:27.761-0600 cloudd  START     seq=0 flags=- run=0
2026-10-28T16:42:27.761-0600 skyd    START     seq=0 flags=- run=0
2026-10-28T16:42:27.762-0600 envd    START     seq=0 flags=- run=0
2026-10-28T16:42:28.766-0600 cloudd  SAMPLE    seq=1 flags=VALID sky=18.750C sensor=18.730C
2026-10-28T16:42:29.766-0600 cloudd  SAMPLE    seq=2 flags=VALID sky=18.710C sensor=18.730C
...
2026-10-28T16:42:32.897-0600 skyd    SAMPLE    seq=1 flags=VALID ch0=929 ch1=226 gain=25x atime=100ms
...
2026-10-28T16:43:37.790-0600 envd    SAMPLE    seq=7 flags=VALID temp=18.610C rh=39.885% pressure=84683Pa
94 records; torn tail: 0 bytes (not modified)
```

**Result:** 94 records = 1 (`stored` START) + 14 + 71 + 8, exactly what the three programs
reported sending. 0 dropped, 0 bad, no `seq` gaps, the 60 s `fsync` ran, every process exited
cleanly on SIGTERM.

## 4. Mechanism D: crash, provoked torn tail, recovery

Two faults back to back while the sensors keep running: a real `kill -9` of `stored`, then a
**provoked** torn tail (20 random bytes appended by hand while `stored` is down, standing in for a
half-written record; it is labeled here as provoked, not observed).

```
### 2026-10-28T16:47:50-06:00 start stored + skyd, cloudd, envd
### 2026-10-28T16:48:11-06:00 kill -9 stored (pid 1532); file is 1368 bytes
### 2026-10-28T16:48:19-06:00 PROVOKED torn tail: appending 20 junk bytes while stored is down
### file is now 1388 bytes
### 2026-10-28T16:48:19-06:00 restart stored
### 2026-10-28T16:48:34-06:00 SIGTERM all
```

`stored.log` (both runs):
```
2026-10-28T16:47:50-0600 386.655 stored[1532] INFO start file=/home/nickmarechal/dsm/data/seg-000001.dsm kept=0 sync_s=60
...
2026-10-28T16:48:19-0600 415.701 stored[1545] WARN recover kept=28 cut_bytes=20
2026-10-28T16:48:19-0600 415.701 stored[1545] INFO start file=/home/nickmarechal/dsm/data/seg-000001.dsm kept=28 sync_s=60
2026-10-28T16:48:20-0600 416.656 stored[1545] INFO client_new clients=1
2026-10-28T16:48:21-0600 417.671 stored[1545] INFO client_new clients=2
2026-10-28T16:48:21-0600 417.779 stored[1545] INFO client_new clients=3
...
2026-10-28T16:48:36-0600 431.849 stored[1545] INFO stop records=21
```

`cloudd.log` (`skyd` and `envd` show the same pattern):
```
2026-10-28T16:48:12-0600 408.655 cloudd[1535] WARN store_disconnect errno=32
2026-10-28T16:48:12-0600 408.656 cloudd[1535] WARN stored_down dropped=1
2026-10-28T16:48:20-0600 416.655 cloudd[1535] INFO store_connect path=/home/nickmarechal/dsm/run/stored.sock dropped=8
2026-10-28T16:48:20-0600 416.656 cloudd[1535] INFO stored_up
2026-10-28T16:48:34-0600 430.809 cloudd[1535] INFO stop records=44 dropped=8
```
`skyd ... stop records=9 dropped=2`; `envd ... stop records=5 dropped=1`.

`dsq dump` afterwards:
```
2026-10-28T16:47:50.808-0600 stored  START     seq=0 flags=- run=0
2026-10-28T16:47:51.804-0600 skyd    START     seq=0 flags=- run=0
2026-10-28T16:47:51.804-0600 cloudd  START     seq=0 flags=- run=0
2026-10-28T16:47:51.804-0600 envd    START     seq=0 flags=- run=0
2026-10-28T16:48:19.854-0600 stored  START     seq=0 flags=- run=0
cloudd seq gap: 20 -> 29
49 records; torn tail: 0 bytes (not modified)
```

**What this shows:**
- **Recovery cut exactly the damage.** Before the crash the file was 1368 B = 24 (header) +
  28 × 48. Recovery kept 28 and cut 20 bytes: the provoked bytes, nothing more.
- **Every record written before the crash survived** a `kill -9`, and appends after recovery
  landed on record boundaries (49 records, 0 torn bytes).
- **The sensors never stopped** (separate processes). Each logged `stored_down`, kept sampling,
  and reconnected by itself within ~1 s of `stored` coming back.
- **Loss is provable, not silent.** `cloudd` reported 8 dropped; the file has the matching
  `seq` gap 20 → 29 (21–28 missing). The second `stored` START marks the restart in the file.

Not yet done for D (planned, spec §9): the ≥ 20 real plug-pulls with the `fsync` bound measured
(DESIGN.md evaluation #11), and the `fsync` interval sweep (#10).

## 5. Valgrind on every program, running together

`tools/valgrind-pipeline.sh 40` runs `stored` and all three sensor programs under
`valgrind --leak-check=full --show-leak-kinds=all` together for 40 s on live sensors, stops them
with SIGTERM, then runs `sensorcheck` the same way:

```
sensor pid 1855 exit=0
sensor pid 1856 exit=0
sensor pid 1857 exit=0
stored exit=0
sensorcheck exit=0
stored       All heap blocks were freed -- no leaks are possible ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
skyd         All heap blocks were freed -- no leaks are possible ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
cloudd       All heap blocks were freed -- no leaks are possible ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
envd         All heap blocks were freed -- no leaks are possible ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
sensorcheck  All heap blocks were freed -- no leaks are possible ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
52 records; torn tail: 0 bytes (not modified)
```

**A real finding, fixed before this run.** The first valgrind run reported
`Syscall param ioctl(I2C_RDWR) points to uninitialised byte(s)` in `src/sensors/i2c.c`: the
compiler's padding bytes inside `struct i2c_msg` and `struct i2c_rdwr_ioctl_data` were not zeroed
by designated initializers, so uninitialized bytes reached the kernel. Fix: `memset` each struct
to zero before filling it. The run above is after the fix.

## Limitations (honest list)

- Indoors only: the MLX90614 "sky" reading is the room, so these runs prove the pipeline, not
  cloud detection.
- One segment file; rotation, `MANIFEST`, write batching and the supervisor (`dskyd`, mechanism E)
  are M4.
- A sensor reconnects only when it next has a record to send, so `skyd` (5 s period) and `envd`
  (10 s) can lose one more sample than `cloudd` after a `stored` restart.
- The Pi has no RTC; it booted with the clock behind until NTP synced (the build printed "clock
  skew"). All timestamps above were taken after `NTPSynchronized=yes`.
