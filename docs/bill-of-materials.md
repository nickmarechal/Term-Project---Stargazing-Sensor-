# Bill of Materials — Dark Sky Monitor

Chosen parts with the reasoning behind each choice, plus the gotchas that cost a week if missed.
Prices are **overestimated** single-unit retail (Oct 2026) — verify current prices before ordering.

## Decisions

### Sky brightness → **TSL2591**

| Option | Verdict |
|---|---|
| **TSL2591** | **Chosen.** 600,000,000:1 dynamic range; minimum detectable ~188 µLux. Programmable gain (up to 9876×) and integration time (up to 600 ms) let us push sensitivity at night. Proven in DIY sky-quality-meter builds. |
| TSL2561 | Rejected — older, much narrower dynamic range. |
| TSL237 (light-to-frequency) | Rejected *for now*. Its pulse output would make brightness an interrupt-timestamping problem (mechanism B), but at bright levels output approaches MHz and would swamp Pi GPIO interrupts, forcing an adaptive gate-counting design. Harder to source on a breakout. Revisit only if we decide we want B. |
| BH1750 / generic lux breakouts | Rejected — ~1 lux resolution floor. Night sky is ~0.001 lux. Off by three orders of magnitude. |

**Bonus we get for free:** the TSL2591 has *two* photodiodes — full-spectrum (Ch0) and infrared (Ch1).
The Ch0/Ch1 ratio hints at light-*source* type, because moonlight, sodium vapour and LED street
lighting have different visible-to-IR ratios. That is a second fusion feature from one sensor, and
a good stretch goal.

**Honest limit:** a truly dark site reads ~0.0002 lux, right at this sensor's floor. In Fort Collins
proper (~0.003 lux) it is comfortable. Mitigation: max gain, 600 ms integration, average many
samples, and characterise the dark offset in a sealed box.

### Cloud cover → **MLX90614** (wide FOV, 3 V-safe breakout)

| Option | Verdict |
|---|---|
| **MLX90614** | **Chosen.** The standard DIY cloud-sensor part. Object temperature range −70 to +380 °C covers cold sky. I2C. |
| MLX90640 (32×24 thermal array) | Rejected — ~$60+, and spatial resolution buys us nothing for a single ΔT measurement. |
| AMG8833 (8×8 thermal) | Rejected — $25–40 for spatial data we don't need. |

**Buy the Adafruit breakout, not a bare clone.** The clones are where the I2C flakiness stories come
from; Adafruit's board includes level shifting and a regulator so it is safe on the Pi's 3.3 V I2C.

**Wide field of view is a feature, not a compromise.** The standard part sees ~90°. Cloud cover is
inherently a spatial average, so averaging a broad patch of sky is what we actually want. A narrow-FOV
variant costs more and is harder to source for no benefit here. Be ready to say this out loud.

### Ambient temp / humidity / pressure → **BME280**

| Option | Verdict |
|---|---|
| **BME280** | **Chosen.** The only single chip giving temperature **+ humidity + pressure** on I2C. Humidity is *required* for the ΔT correction; pressure is a free bonus for front detection. |
| BMP280 | Rejected — **no humidity sensor.** Humidity is the whole reason this part exists in our design. |
| SHT31 / SHT41 | Rejected — better humidity accuracy but no pressure, so we'd need a fourth part. |
| DHT22 | Rejected — bit-banged microsecond timing from userspace, slow, coarse. The spec itself lists it as "a lesson in why kernels exist." |

### Clock → **DS3231**

Required, not optional: no network means **no NTP**, and the Pi has no battery-backed clock. Without
it we cannot know whether it is astronomically dark, and the sun/moon ephemeris — the component
carrying most of this project's sophistication — is impossible.

DS3231 over DS1307 because the DS3231 is temperature-compensated (±2 ppm vs ~±20 ppm) for the same money.

### Computer → **Raspberry Pi 4, 2 GB**

- **2 GB is plenty.** The workload is three sensors at ≤1 Hz. Do not pay for 4–8 GB.
- **Pi 4 over Pi 5** for a concrete reason: the Pi 5 moved GPIO behind a new RP1 chip, which is newer
  and less documented. If we attempt mechanism A (a character driver), the Pi 4's better-documented
  peripherals matter. The Pi 4 also runs cooler, which matters for a sealed box left outside.
- **Pi 4 over Zero 2 W** only because we compile on it. A Zero 2 W would *run* this fine.
- A used Pi 4 or 3B+ off Marketplace/eBay (~$35) is a legitimate way to save $20.

## The build list

| # | Part | Why this exact one | Est. |
|---|---|---|---|
| 1 | Raspberry Pi 4 (2 GB) | see above | $55 |
| 2 | **High-endurance** microSD 32 GB (SanDisk High Endurance / Samsung PRO Endurance) | We write continuously for weeks. These are built for dashcams — continuous writes — and cost ~$3 more than a card that will wear out. 32 GB is plenty. | $14 |
| 3 | Official Pi USB-C PSU (5.1 V 3 A) | **Not a phone charger.** Undervoltage on a Pi causes random instability and SD corruption, and it looks exactly like a software bug. Worst possible week to debug. | $10 |
| 4 | TSL2591 breakout (Adafruit #1980 or equiv.) | sky brightness | $12 |
| 5 | MLX90614 breakout (Adafruit #1747 or equiv.) | cloud via sky IR temperature | $20 |
| 6 | BME280 breakout (Adafruit #2652 / SparkFun) | ambient + **humidity** + pressure | $16 |
| 7 | DS3231 RTC + CR2032 or LIR2032 | offline timekeeping | $10 |
| 8 | Breadboard + female-female jumper wires | prototype before enclosing | $14 |
| 9 | IP65 project box / junction box (Home Depot, Lowe's) | enclosure | $14 |
| 10 | Clear acrylic or glass scrap (brightness window), thin LDPE film, standoffs, screws | see enclosure notes | $10 |
| | **Recommended total** | | **~$175** |

### Lean version (~$121, ≈ $60 each)

Used Pi 4/3B+ ($35) · SD ($14) · PSU ($10) · TSL2591 clone ($11) · MLX90614 clone ($13) · BME280
($9, **verify chip ID**) · DS3231 ($7) · wire kit ($12) · junction box ($10).

### Optional, strongly recommended later

| Part | Why | Est. |
|---|---|---|
| Power resistor (~10–22 Ω) + MOSFET + wire | Dew heater for the IR aperture. Turns failure mode #4 into a feature and adds a measurable control loop. | $8 |
| 3D-printed sensor shroud | **Ask whether CSU has a makerspace printer** — likely free. | $0 |

## Where to buy

| Source | Speed to Fort Collins | Use it for |
|---|---|---|
| **CSU ECE stockroom / makerspace** | **Today** | Ask first. Free beats fast. |
| **SparkFun** (Niwot, CO — ~45 min away) | 1–2 days ground, in-state | **Best paid option.** Authentic parts, genuinely local. |
| **Amazon Prime** | 1–2 days | Whatever the above can't cover. Cheapest, but see the counterfeit warning. |
| **Adafruit** (NYC) | 3–7 days standard | Best quality + documentation. Use if the timeline allows. |
| **DigiKey / Mouser** | 1–2 days | Guaranteed-authentic parts. |

Raspberry Pi itself: an authorised reseller, or used locally to save $20.

## Gotchas that cost a week if missed

1. **Counterfeit BME280s are rampant.** Boards sold as "BME280" are frequently BMP280 (no humidity) —
   and humidity is the entire reason we bought it. **Verify by reading the chip-ID register: 0x60 =
   BME280, 0x58 = BMP280.** Do this the day the part arrives, not in week 11. Buying from SparkFun or
   Adafruit avoids the problem.
2. **The MLX90614 cannot see through acrylic.** Acrylic and most plastics are opaque in the 8–14 µm
   band it uses, so a sealed clear window **blinds the cloud sensor** — half of our fusion. Options:
   an open aperture under an offset rain hood tilted ~15° for drainage, or a **thin LDPE film**
   window (polyethylene is reasonably LWIR-transparent — cling film or a bag, calibrated in place,
   replaced periodically). **Test this in the first week the part is in hand.**
3. **The MLX90614 is finicky on the Pi's hardware I2C** (SMBus repeated-start behaviour). Two fixes,
   and the second is better: lower the bus speed (`dtparam=i2c_arm_baudrate=50000` in
   `/boot/firmware/config.txt`), **or** put the MLX90614 on its own bit-banged `i2c-gpio` bus on
   separate pins. The second option solves two problems at once — it dodges the SMBus quirk *and*
   removes this sensor from bus contention with the other three (see the I2C contention discussion in
   `design-review.md` §5).
4. **Cheap DS3231 modules (ZS-042) have a trickle-charge circuit** intended for a rechargeable
   LIR2032. Fitting a non-rechargeable CR2032 means the board tries to charge it — a leak/rupture
   risk. Either fit a LIR2032, remove the charging resistor, or buy the Adafruit breakout which
   doesn't do this.
5. **Keep I2C cable runs under ~30 cm.** I2C is not a long-distance bus; cable capacitance causes
   corruption that looks like random sensor failure. If the sensor head must sit further from the Pi,
   lower the baudrate and expect to debug it.

## Wiring

All four devices are I2C and **their addresses do not collide** — worth stating explicitly, because
address conflicts are a classic time sink:

| Device | Address |
|---|---|
| TSL2591 | 0x29 |
| MLX90614 | 0x5A |
| BME280 | 0x76 (or 0x77) |
| DS3231 | 0x68 (+ 0x57 if the module carries an AT24C32 EEPROM) |

- Power everything from the Pi's **3.3 V** pin where the breakout allows it. Nothing here needs 5 V.
- Pi I2C is on **GPIO2 (SDA)** and **GPIO3 (SCL)**, which have built-in 1.8 kΩ pull-ups. Four breakouts
  each adding their own pull-ups in parallel lowers the effective resistance; usually fine at 100 kHz,
  but if the bus misbehaves this is a place to look.
- **Wire with the Pi powered off.** A Pi killed by a hot-plugged jumper in week 11 is a schedule
  event, not a hardware event.
- Confirm everything enumerates with `i2cdetect -y 1` before writing a line of C.

## Enclosure and placement

- **Sensor head points at the zenith**, as far from walls, roof overhangs and windows as practical —
  anything in the field of view radiates at its own temperature and corrupts ΔT.
- **TSL2591:** behind a clear acrylic or glass window. **Calibrate with the window in place** so its
  transmission folds into the calibration constant. Note that acrylic yellows under UV over months.
- **MLX90614:** open aperture or LDPE film, under an offset hood, tilted ~15° so water runs off.
- **BME280:** shaded and vented. Direct sun or trapped case heat makes its temperature reading useless,
  and that reading is the ambient reference for every ΔT.
- **Pi and RTC:** inside the sealed part of the box. The Pi's own heat is a feature in a Colorado
  November.
