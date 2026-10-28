# Hardware log

Bring-up evidence for the Dark Sky Monitor. **Paste real tool output here, not descriptions of it.**

Team rule from `CLAUDE.md`, and it exists because the agent will confidently reason about a sensor
it has never met and be wrong in ways only an instrument catches: never accept a fix for a hardware
problem that was diagnosed from a verbal description. `dmesg`, `i2cdetect`, timing captures,
`/proc/interrupts` — paste the actual bytes.

## Device of record

Each partner has a kit for parallel development: **`darksky.local`** (Nick's Pi 4 1 GB + sensors)
and **`starlight.local`** (Liam's dev Pi + TSL2591, MLX90614, BME280). **Numbers are reported
only from the device of record, `darksky.local`:** the soak, the demo, and every number in
`EVALUATION.md`. Output from `starlight.local` may be logged here for debugging, always labeled
with its hostname. Differences in parts between the kits (breakout vendor or revision, BME280
chip ID, MLX90614 part suffix, I2C addresses) are logged under "Kit differences" as they are
found.

| Bring-up step (sections below) | `darksky.local` (device of record) | `starlight.local` (dev only) |
|---|---|---|
| §0 Pi set up: SSH, I2C on, `get_throttled` | done 2026-10-27 | |
| §1 `i2cdetect -y 1` | | |
| §2 BME280 chip ID | | |
| §3 MLX90614 sky vs. wall | | |
| §4 window material | | |
| §5 TSL2591 dark offset | | |

**Kit differences:** none recorded yet. Neither kit is wired (2026-10-27).

## Status

| Item | Ordered | Arrived | Verified | Notes |
|---|---|---|---|---|
| Raspberry Pi 4 (1 GB) | 2026-10-06 | 2026-10-13 | 2026-10-27 | PiShop.us, $35. Boots, SSH, I2C on — see §0 |
| TSL2591 | 2026-10-06 | 2026-10-13 | | Amazon, EC Buying, 3 V/5 V. Not wired yet |
| MLX90614 / GY-906 | 2026-10-06 | 2026-10-13 | | Amazon, **3.3 V pack**. Not wired yet |
| BME280 | 2026-10-06 | 2026-10-13 | | Amazon, incl. jumper wires. Not wired yet |
| USB-C power supply | | by 2026-10-27 | 2026-10-27 | `throttled=0x0` — see §0. On a dual-port charger use one port only: the two ports share 3 A |
| microSD | | by 2026-10-27 | 2026-10-27 | Patriot LX 16 GB. Standard card: logging writes ~13 MB/day, so a high-endurance card isn't needed |
| USB-C microSD reader | | 2026-10-27 | 2026-10-27 | Only needed to flash the card from a Mac without an SD slot |
| Breadboard + jumper wires | | | | Wiring planned for 2026-10-27 evening |
| DS3231 RTC | to be ordered | | | Until fitted, the Pi gets real time from NTP only, so it must boot on a network with internet |

## Known gap: M2 "both sensors electrically alive" not met (2026-10-27)

At the M2 deadline (2026-10-28) neither kit is wired, so §1–5 below hold no evidence yet, and every
value marked (a) in `DESIGN.md` is a datasheet assumption, not a measurement. **Plan:** wire
`darksky.local` and run §1–5 **by 2026-11-03**, paste the raw output here, then correct `DESIGN.md`
from it. Nothing in this log may be filled in from memory or a description: only tool output.

## 0. Pi setup (2026-10-27, headless — no monitor or keyboard)

- Raspberry Pi Imager v2.0.12 on macOS → **Raspberry Pi OS Lite (64-bit)**. Settings applied by
  Imager through cloud-init (`user-data`, `network-config` on the boot partition): hostname
  `darksky`, SSH with password login, timezone `America/Denver`, Wi-Fi region `US`.
- Reached from the Mac as `darksky.local` (mDNS) about 50 s after first power-on; SSH port open
  about 10 s after that.
- `sudo apt update && sudo apt full-upgrade -y`, then
  `sudo apt install -y git build-essential i2c-tools valgrind rsync` — no errors.
- I2C enabled with `sudo raspi-config nonint do_i2c 0`, then reboot.

```
$ vcgencmd get_throttled
throttled=0x0
$ ls /dev/i2c-1
/dev/i2c-1
```

`0x0` = no undervoltage and no throttling, now or since boot. `/dev/i2c-1` = the I2C bus the
sensors will sit on is switched on.

First build of the project on Linux (code copied with `make deploy`, Steps 0–2 of the storage code):

```
$ cd ~/term-project && make test && make memcheck
gcc -std=c17 -Wall -Wextra -Werror -g -O1 -Isrc -Itests -D_POSIX_C_SOURCE=200809L src/common/crc32.c src/common/record.c tests/test_main.c tests/test_crc32.c tests/test_record.c -o build/test_all
./build/test_all
15 checks, 0 failed
==1149== Memcheck, a memory error detector
==1149== Using Valgrind-3.24.0 and LibVEX; rerun with -h for copyright info
==1149== Command: ./build/test_all
15 checks, 0 failed
==1149== HEAP SUMMARY:
==1149==     in use at exit: 0 bytes in 0 blocks
==1149==   total heap usage: 1 allocs, 1 frees, 1,024 bytes allocated
==1149== All heap blocks were freed -- no leaks are possible
==1149== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
```

The one 1,024-byte allocation is glibc's stdout buffer for `printf`, not project code.

## 1. I2C enumeration

Expect 0x29 (TSL2591), 0x5A (MLX90614), 0x76 or 0x77 (BME280), 0x68 (DS3231 once fitted).

```
$ i2cdetect -y 1
(paste output)
```

**Result:**

## 2. BME280 chip ID — is it genuine?

`0x60` = BME280 (has humidity). `0x58` = BMP280 (no humidity) → **return the part.**

```
$ i2cget -y 1 0x76 0xD0
(paste output)
```

**Result:**

## 3. MLX90614 sky vs. indoor wall

A clear zenith should read tens of °C below ambient. A wall should read near ambient. If both are
the same, something is in the field of view or the sensor is bad.

| Target | T_object | T_ambient | ΔT |
|---|---|---|---|
| Clear night sky (zenith) | | | |
| Indoor wall | | | |
| Overcast sky | | | |

**Result:**

## 4. Window material test — the project's biggest hardware unknown

Acrylic is expected to be **opaque** in the MLX90614's 8–14 µm band, which would mean a sealed clear
window blinds the cloud sensor entirely. Thin polyethylene film is expected to pass LWIR.

| Window in front of sensor | T_object reading | Cold-sky signal survives? |
|---|---|---|
| Nothing (open aperture) | | baseline |
| Acrylic / clear plastic | | |
| Thin LDPE film (cling film / bag) | | |
| Glass | | |

**Result and decision on enclosure aperture:**

## 5. TSL2591 dark offset

Max gain (9876×), 600 ms integration, sensor sealed in a light-tight box. This is the noise floor
every night-sky reading sits on, so it has to be characterised before any brightness claim.

```
(paste readings)
```

**Result:**

## 6. DS3231 timekeeping across power loss

Set the time, power the Pi off completely for 10+ minutes, boot with no network, confirm the clock
is still correct.

**Result:**

## Problems encountered

Log anything that cost more than ten minutes, with the evidence that diagnosed it. Known candidates
from the design review: the MLX90614's SMBus quirk on Pi hardware I2C (fixes are lowering
`i2c_arm_baudrate` or moving it to a bit-banged `i2c-gpio` bus), and I2C cable length.
