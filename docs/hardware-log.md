# Hardware log

Bring-up evidence for the Dark Sky Monitor. **Paste real tool output here, not descriptions of it.**

Team rule from `CLAUDE.md`, and it exists because the agent will confidently reason about a sensor
it has never met and be wrong in ways only an instrument catches: never accept a fix for a hardware
problem that was diagnosed from a verbal description. `dmesg`, `i2cdetect`, timing captures,
`/proc/interrupts` — paste the actual bytes.

## Status

| Item | Ordered | Arrived | Verified | Notes |
|---|---|---|---|---|
| Raspberry Pi 4 (1 GB) | 2026-10-06 | | | PiShop.us, $35 |
| TSL2591 | 2026-10-06 | | | Amazon, EC Buying, 3 V/5 V |
| MLX90614 / GY-906 | 2026-10-06 | | | Amazon, **3.3 V pack** |
| BME280 | 2026-10-06 | | | Amazon, incl. jumper wires |
| USB-C PSU (5.1 V 3 A) | | | | **not ordered — Pi will not boot without it** |
| microSD (high endurance) | | | | **not ordered** |
| DS3231 RTC | | | | **not ordered** |

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
