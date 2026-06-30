# Giga Ranger — Design Log

60 km point-to-point SX1280 ranging link. Two fixed sites, LOS, directional antennas.

---

## Hardware

| Component | Detail |
|---|---|
| Module | LILYGO T3-S3, SX1280 **without PA** |
| Antenna | 2.4 GHz 13 dBi Yagi, SMA-female, VNA-verified SWR 1.11 @ 2.45 GHz |
| Environmental | BME280 (I²C) for atmospheric delay correction |
| Power | RECOM R-78E5.0-1.0 (12V→5V) for field/solar |

**Non-PA version is required.** External PA breaks the ranging turnaround timing — the SX1280 must control TX/RX switching internally.

---

## Radio Configuration

| Parameter | Value |
|---|---|
| Frequency | 2450 MHz |
| Bandwidth | 1625 kHz (maximum — always use max BW for best ranging precision) |
| Spreading Factor | SF9 (see SF selection below) |
| TX Power | 13 dBm |
| Mode | One initiator (Master) + one responder (Slave) |

---

## Link Budget

| Item | Value |
|---|---|
| TX power | +13 dBm |
| TX antenna gain | +13 dBi |
| RX antenna gain | +13 dBi |
| Free-space path loss @ 2.45 GHz, 60 km | −136 dB |
| Misc losses (cable, connector, pointing) | −4 dB |
| **Received signal** | **−101 dBm** |
| SX1280 sensitivity (SF9, BW=1625 kHz) | −116 dBm |
| **Link margin** | **~15 dB** |

---

## SF Selection — 2026-06-14

**Selected: SF9**

For ranging, the precision trade-off is the **opposite** of data comms: lower SF = better precision, not worse. The constraint is link closure, not data rate.

### Precision vs SF at BW=1625 kHz (Cramer-Rao Lower Bound, theoretical LoS)

Source: Semtech AN1200.89 *Theory and Principle of Advanced Ranging*, v2.0, April 2024.

| SF | Theoretical σ | Link margin @ −101 dBm RX |
|----|---------------|--------------------------|
| SF7 | ~0.15 m | ~9 dB (marginal) |
| SF8 | ~0.2 m | ~12 dB |
| **SF9** | **~0.3–0.5 m** | **~15 dB (solid)** |
| SF10 | ~0.5–1 m | ~18 dB |
| SF12 | ~2–3 m | ~24 dB |

SF9 gives solid link margin while staying low enough for good ranging precision (~0.4 m theoretical). SF7/SF8 risk marginal closure under atmospheric variation at 60 km. SF10+ wastes precision unnecessarily.

**Revisit:** If on-site signal is substantially stronger than calculated, try SF8 and compare precision empirically.

---

## Ranging Physics (Semtech AN1200.89)

### Conventional RTToF (the mode used here)

- Master sends ranging request, starts internal timer
- Slave re-transmits a synchronized response after hardware-calibrated delay
- Round trip = 2×T_MS; chip calibrates out processing delays internally
- Output: single distance value (radius from Master to Slave)
- Apply `CALIBRATION_OFFSET_M` constant after a known-distance baseline run

### Precision (theoretical LoS, CRLB)

- Higher BW and lower SF always improve ranging precision
- BW=1625 kHz is the SX1280 maximum — always use it for ranging
- Theoretical best-case at SF9/BW=1625 kHz: σ ≈ 0.3–0.5 m
- Real-world precision will be coarser; calibration removes systematic offset but not noise floor

### Multipath

- Reflected paths are always longer than LoS → multipath always **over-estimates** range
- Most severe in nLoS environments (indoor, obstructed)
- At 60 km LoS with narrow-beam Yagi antennas: multipath is not a significant concern

### Advanced Ranging (not used — documented for reference)

A third SX1280 in Advanced Ranging mode can passively overhear an existing Master-Slave exchange without transmitting. It measures ΔT = T_MS + T_SA − T_MA, placing itself on a hyperbola between the two active nodes. Enables unlimited passive listeners from a single ranging exchange. Not applicable to the two-node point-to-point Giga Ranger configuration.

---

## Software Architecture

```
Rust (esp-idf-sys / std)
    └── calls → sx1280.h bindings (bindgen)
                    └── sx1280.c  (Semtech reference driver)
                            └── sx1280-hal.h callbacks
                                    └── sx1280_hal.c  (IDF SPI/GPIO — our work)
```

C owns everything the radio touches. Rust owns init, ranging loop, calibration math, display, serial output.

### Pin assignments (LILYGO T3-S3 — to be confirmed on hardware arrival)

Schematic: https://github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series/blob/master/schematic/T3_S3_V1.2.pdf

| Signal | GPIO | Notes |
|---|---|---|
| NSS | TBD | SPI chip select |
| BUSY | TBD | Must poll before SPI |
| DIO1 | TBD | Ranging-done interrupt |
| RST | TBD | Active-low reset |
| SCK/MOSI/MISO | TBD | SPI bus |

---

## Calibration Plan

1. Deploy both nodes at a location with a precisely known separation
2. Collect 20+ ranging results
3. `CALIBRATION_OFFSET_M = mean(raw_readings) - known_distance_m`
4. Confirm with a second known-distance point

GPS coordinates are not used for distance reference. Methodology is projection-agnostic — physical measurement only.

---

## Status

- [ ] Hardware arrived
- [ ] Pin assignments confirmed
- [ ] C HAL shims written and tested
- [ ] Bench ranging (1–10 m) verified
- [ ] Field deployment
- [ ] Calibration run complete
