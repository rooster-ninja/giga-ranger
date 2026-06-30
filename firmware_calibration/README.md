<p align="center">
  <img src="../Assets/GooseLogo.png" alt="Goose" width="320"/>
</p>

<h1 align="center">Giga Ranger — Calibration Firmware</h1>
<p align="center"><em>SX1280 ranging calibration for LILYGO T3-S3 V1.3</em></p>

---

## Flash Instructions

**1. Flash the slave board**
```bash
cd ~/firmware_calibration
~/.local/bin/pio run -e slave -t upload
```

**2. Flash the master board**
```bash
~/.local/bin/pio run -e master -t upload
```

**3. Open serial monitor on master (115200 baud)**
```bash
~/.local/bin/pio device monitor
```

---

## Calibration Procedure

Assemble the signal chain before opening the monitor:

```
[T3-S3 Master] ── SMA ── [40 dB atten] ── [1 m RG-316] ── [40 dB atten] ── SMA ── [T3-S3 Slave]
```

Both boards inside steel chassis. **80 dB total attenuation required.**

| Step | Action |
|---|---|
| Run A | Flash and connect as above. Master collects 500 exchanges → prints `CalibrationValue_A` |
| Run B | Swap board roles (re-flash `-e master` / `-e slave`), repeat → `CalibrationValue_B` |
| Final | `CalibrationValue = (A + B) / 2` — averages out TX/RX path asymmetry |

Write the result to production firmware:
```cpp
radio.setRangingCalibration(YOUR_AVERAGED_VALUE);
```

---

## Cable Constants

| Property | Value |
|---|---|
| Part | DigiKey J10302-ND / 415-0031-M1.0 |
| Type | RG-316 MIL-DTL-17, Amphenol CIT — confirmed from jacket |
| Physical length | 1.000 m |
| Velocity factor | 0.695 |
| Electrical length | 0.695 m (3.86 ranging counts) |

---

## Pin Assignments (T3-S3 V1.3, SX1280)

Confirmed from `T3_S3_V1.3_schematic.pdf`.

| Signal | GPIO |
|---|---|
| CS / NSS | 7 |
| SCK | 5 |
| MOSI | 6 |
| MISO | 3 |
| DIO1 | 33 |
| BUSY | 34 |
| RESET | 8 |

---

## Build Environment

Verified on Arch Linux x86_64.

| Component | Version |
|---|---|
| PlatformIO | 6.1.19 |
| RadioLib | 7.7.1 |
| espressif32 platform | 7.0.1 |
| Framework | Arduino |

| Environment | Status | RAM | Flash |
|---|---|---|---|
| master | ✅ SUCCESS | 6.1% | 8.8% |
| slave | ✅ SUCCESS | 6.1% | 8.8% |
