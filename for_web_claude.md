# Giga Ranger — Hardware & Operation Theory Summary

> Reference document for Claude Code web development tasks. Covers what the system does, how it works, and the physical principles behind the measurements.

---

## What It Is

A two-node **radio ranging link** that measures the physical distance between two fixed sites using the speed of light. No GPS involved in the measurement. The system exploits the SX1280's built-in time-of-flight hardware to produce a distance reading accurate to sub-meter precision over a ~60 km line-of-sight path in British Columbia.

Target deployment: one node on a mountaintop, one at a valley station. Both sites are fixed; the distance between them changes only as calibration verifies.

---

## Hardware (per site — build two identical)

| Component | Part | Notes |
|---|---|---|
| MCU + Radio | LILYGO T3-S3, SX1280 **without PA** | ESP32-S3 + SX1280 integrated. **Non-PA version is mandatory** — external PA breaks the ranging turnaround timing. The SX1280 must control TX/RX switching internally. |
| Antenna | 2.4 GHz 13 dBi Yagi, SMA-female | VNA-verified: SWR 1.11 @ 2.45 GHz. Direct mate to T3-S3 SMA jack — no adapter or pigtail. Beamwidth E=48° / H=75°. Realized gain ~11 dBi. |
| Weather sensor | BME280 (I²C) | Must be BME280 (has humidity), not BMP280. Feeds the atmospheric delay correction model. |
| Power | RECOM R-78E5.0-1.0 | 12V → 5V switching regulator for solar/battery field power. Clean, efficient, drop-in. |
| Enclosure | IP65 plastic box | RF-transparent (plastic, not metal). Weatherproof SMA joint. |

---

## Radio Configuration

| Parameter | Value | Reason |
|---|---|---|
| Frequency | 2450 MHz | Centre of 2.4 GHz ISM band |
| Bandwidth | 1625 kHz | Maximum available — always use max BW for best ranging precision |
| Spreading Factor | SF9 | Balances precision and link margin (see SF table below) |
| TX Power | 13 dBm | Chip maximum |
| Roles | One Master (initiator) + one Slave (responder) | Identical RF params on both nodes required |

### SF Selection Trade-off

For ranging, lower SF = better precision (opposite of data comms). The constraint is link closure, not throughput.

| SF | Theoretical σ | Link margin @ −101 dBm RX |
|---|---|---|
| SF7 | ~0.15 m | ~9 dB — marginal |
| SF8 | ~0.2 m | ~12 dB |
| **SF9** | **~0.3–0.5 m** | **~15 dB — selected** |
| SF10 | ~0.5–1 m | ~18 dB |
| SF12 | ~2–3 m | ~24 dB |

SF9 gives solid margin while staying low enough for good precision. SF7/SF8 risk marginal closure under atmospheric variation at 60 km.

---

## Link Budget

| Item | Value |
|---|---|
| TX power | +13 dBm |
| TX antenna gain | +13 dBi |
| RX antenna gain | +13 dBi |
| Free-space path loss @ 2.45 GHz, 60 km | −136 dB |
| Cable, connector, pointing/polarization losses | −4 dB |
| **Received signal** | **−101 dBm** |
| SX1280 sensitivity (SF9, BW=1625 kHz) | −116 dBm |
| **Link margin** | **~15 dB** |

Empirical baseline: StuartsProjects achieved 40 km at SF10/BW406 with 2 dBi antennas at 4 dBm TX — back-calculated effective system sensitivity ~−124 dBm. The 60 km link adds only ~3.5 dB more path loss; the 13 dBi Yagis and 13 dBm TX give substantial headroom.

**Radio horizon (not link budget) is the real constraint** at 60 km — antenna height and terrain clearance at the midpoint matter more than raw power.

---

## How Ranging Works (Operation Theory)

### Round-Trip Time-of-Flight

The SX1280 uses a two-packet exchange between Master and Slave:

1. **Master** sends a ranging request and starts an internal hardware timer
2. **Slave** receives it and re-transmits a synchronised response after a hardware-calibrated fixed delay
3. **Master** receives the response and stops the timer
4. Round-trip time = 2 × T_propagation; the chip subtracts the known Slave processing delay internally
5. Output: raw 24-bit `RangingResult` register value, converted to metres via **0.1803 m/count** (empirically derived from SX1280 40 km field test)

The SX1280 hardware handles all timing internally — no application-layer timestamping needed.

### CalibrationValue (RxTxDelay Register)

Fixed hardware delays in the radio front end (PCB traces, TX/RX switching, front-end group delay) add a constant bias to every ranging result. This is corrected by characterising the bias once and writing it to the `RxTxDelay` register:

```
CalibrationValue = R_measured − R_cable
R_cable = D_cable / 0.1803          (cable electrical length in ranging result units)
D_cable = physical_length × velocity_factor
```

Once written, the SX1280 applies the correction automatically. The value is **SF-dependent** and **board-design-dependent** — must be measured empirically for each hardware revision and SF in use.

### Role Reversal (Asymmetric Delay Cancellation)

TX and RX path delays are not identical on the same board. To cancel the asymmetric contribution, calibration measurements are averaged with **Master and Slave roles swapped**:

```
R_measured = (raw_role_A.mean() + raw_role_B.mean()) / 2
```

Wolf et al. (2019) collected >1000 exchanges per configuration to reduce standard error to <0.016 m — negligible relative to the ~0.5 m per-measurement noise floor.

### Achieved Precision

Wolf et al. (2019) benchmark the SX1280 in a cabled AWGN channel and confirm **σ = 0.5 m** at BW=1625 kHz after bias removal. This is the noise floor for Giga Ranger's configuration.

---

## Atmospheric Delay Correction (GRIN Model)

The lower atmosphere is a gradient-index (GRIN) medium: refractive index decreases with altitude as air density drops. This causes two effects on the ranging link:

1. **Path bending** — signal curves slightly downward (extends effective radio horizon; extends beyond geometric LOS). Amounts to only centimetres of extra path at 60 km — negligible for correction.

2. **Signal slowing** — because n > 1, the signal travels slower than *c*, adding excess electrical path length that the SX1280 reports as extra distance. Over 60 km this excess is approximately **15 m (~50 ns one-way)**. The weather-driven *variation* in this excess is ~1 m — right at the precision target.

### Correction Formula

```
ΔL = 10⁻⁶ ∫ N(h) ds       (integral of refractivity along the path chord)

N = 77.6·P/T  +  3.73×10⁵·e/T²     (ITU-R P.453: dry + wet terms)
```

Where P = pressure (hPa), T = temperature (K), e = water vapour partial pressure (hPa from BME280 RH).

### Calibration-Day Reference

The surveyed-distance calibration already absorbs the atmosphere as it was on calibration day. In operation, subtract only the **change** vs calibration conditions — not the full 15 m bias (that would double-count):

```rust
let ref_excess = excess_delay(..., p_cal, t_cal, rh_cal, ...).excess_path_m;
let now        = excess_delay(..., p_now, t_now, rh_now, ...).excess_path_m;
let dynamic_corr = now - ref_excess;
let true_distance = measured - survey_offset - dynamic_corr;
```

The BME280 at each station feeds live P, T, RH into this model continuously.

### Known Limitation

The exponential density profile breaks down under **ducting / super-refraction** — strong temperature inversions or a path skimming water can bend the signal anomalously. A mountaintop-to-valley path may cross inversions on calm mornings. Anomalous ranging jumps under stable cool conditions suggest ducting.

---

## Software Architecture

```
Rust (esp-idf-sys / std)
    └── FFI → sx1280.h bindings (bindgen)
                └── sx1280.c  (Semtech reference C driver)
                        └── sx1280-hal.h callbacks
                                └── sx1280_hal.c  (ESP-IDF SPI/GPIO — project HAL)
```

C owns everything the radio touches. Rust owns init, ranging loop, calibration math, BME280 reads, atmospheric correction, display, and serial output.

---

## Install Notes

- **Polarisation:** Yagi polarisation follows element orientation. Both ends must match (both vertical or both horizontal) — 90° mismatch causes ~−20 dB loss. Decide once and note on both install sheets.
- **Aiming:** Pre-aim by GPS great-circle bearing, correcting for ~14–15° E magnetic declination in eastern BC. Fine-peak on RSSI from the SX1280's live reading.
- **Diagnostics:** If RSSI is low despite good aim — rotate one antenna 90° (polarisation check first), then verify Fresnel-zone and earth-curvature clearance at the midpoint.
- **Multipath:** Reflected paths are always longer than LOS → multipath always over-estimates range. At 60 km LOS with narrow-beam Yagis this is not a significant concern.

---

## Key References

| Document | Description |
|---|---|
| AN1200.29 | Introduction to Ranging with the SX1280 — calibration setup, RxTxDelay register, role-reversal procedure |
| AN1200.50 | SX1280/SX1281 Datasheet Rev 2.3, Sept 2023 — §14.5 Ranging Engine, Table 14-60 CalibrationValue register |
| Wolf et al. (2019) | WPNC2019 — SX1280 ranging benchmark; confirms σ=0.5m at BW=1625 kHz after bias removal |
| StuartsProjects (2019) | 40 km SX1280 field test — source of 0.1803 m/count empirical conversion factor |
| AN1200.89 | Theory and Principle of Advanced Ranging — CRLB precision vs SF/BW, Advanced Ranging hyperbolic mode |

All PDFs available at: `https://rooster.ninja/skunk_werks/giga_ranger/assets/`

---

## Project Status (as of June 2026)

- [ ] Hardware arrived
- [ ] T3-S3 pin assignments confirmed
- [ ] C HAL shims written and tested
- [ ] Bench ranging (1–10 m) verified
- [ ] Conducted calibration complete (1 m coax + 80 dB attenuation setup)
- [ ] Field deployment
- [ ] Calibration run at operating temperature
