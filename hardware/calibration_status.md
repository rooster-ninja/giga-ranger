# Giga Ranger — Calibration Status

_Last updated: 2026-06-29_

---

## Hardware

| Item | Status | Notes |
|---|---|---|
| LILYGO T3-S3 V1.3 boards (×2) | **Arrived** | SX1280 "Ranging / LoRa 2.4G" variant |
| Hammond 1441-12 steel chassis (×2) | **Arrived** | Shielded enclosures for conducted cal |
| Hammond 1431-12 chassis covers (×2) | **Arrived** | |
| 3M 1125 copper foil shielding tape | **Arrived** | Seam sealing |
| RG-316 1 m SMA-M/SMA-M cable | **Arrived** | DigiKey J10302-ND; VF=0.695; elec. length = 0.695 m |
| 40 dB SMA attenuators (×2) | **Ordered — not arrived** | Pasternack; need both for 80 dB total |
| Right-angle SMA board connectors | **To order** | For chassis/board interconnect |

---

## To Order

- **Right-angle SMA connectors** — board-mount, for routing RF inside the Hammond chassis to the bulkhead or attenuator chain

---

## Software

| Item | Status |
|---|---|
| Calibration firmware | **Written** — `firmware_calibration/` |
| T3-S3 V1.3 pin assignments | **All confirmed** from schematic. See `Assets/T3S3_V1.3_SX1280_pins.md` |
| Production firmware (ranging loop, GRIN, web UI) | Not started |

---

## Calibration Procedure (when attenuators arrive)

Signal chain:
```
[T3-S3 Master] ── SMA ── [40 dB atten] ── [1 m RG-316] ── [40 dB atten] ── SMA ── [T3-S3 Slave]
```
Both boards inside steel chassis. 80 dB total attenuation.

**Steps:**

1. Flash master: `pio run -e master -t upload`
2. Flash slave:  `pio run -e slave  -t upload`
3. Connect signal chain, open serial monitor on master
4. 500 ranging exchanges run automatically → `CalibrationValue_A` printed
5. Swap roles (re-flash), re-run → `CalibrationValue_B`
6. `CalibrationValue = (A + B) / 2`  _(role reversal cancels TX/RX path asymmetry)_
7. Write to production firmware: `radio.setRangingCalibration(CalibrationValue)`

**Cable constants:**
- Physical: 1.000 m
- VF: 0.695 (RG-316 MIL-DTL-17, Amphenol CIT — confirmed from jacket)
- Electrical: 0.695 m → 3.86 ranging counts

---

## Immediate Next Steps

1. Order right-angle SMA board connectors
2. Verify BUSY = GPIO34 on T3-S3 V1.3 schematic
3. Build-check firmware: `cd firmware_calibration && pio run -e master`
4. Wait for attenuators → run calibration procedure
