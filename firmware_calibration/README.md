<p align="center">
  <img src="../Assets/GooseLogo.png" alt="Goose" width="320"/>
</p>

<h1 align="center">Giga Ranger — Calibration Firmware</h1>
<p align="center"><em>SX1280 ranging calibration for LILYGO T3-S3 V1.3</em></p>

---

## Flash Instructions

PlatformIO must be installed (`pip install platformio` or via Homebrew).

**1. Flash the slave board**
```bash
cd firmware_calibration
pio run -e slave -t upload --upload-port /dev/cu.usbmodem<SLAVE>
```

**2. Flash the master board**
```bash
pio run -e master -t upload --upload-port /dev/cu.usbmodem<MASTER>
```

**3. Open serial monitor on master (115200 baud)**
```bash
pio device monitor --port /dev/cu.usbmodem<MASTER>
```

Press RST on the master board — output appears after the 3-second startup delay.

**Tip — find port:** `ls /dev/cu.*`

---

## Calibration Procedure

Assemble the signal chain before opening the monitor:

```
[T3-S3 Master] ── SMA ── [40 dB atten] ── [1 m RG-316] ── SMA ── [T3-S3 Slave]
```

> **Note:** One 40 dB attenuator is sufficient at −18 dBm TX (−58 dBm at RX).
> **Warning:** Never exceed +5 dBm — board has a PA FEM that will be damaged.

| Step | Action |
|---|---|
| Run A | Flash as above, connect cable+atten. Master collects 500 exchanges → prints `CalibrationValue_A` |
| Run B | Swap board roles (re-flash `-e master` / `-e slave` on opposite boards), repeat → `CalibrationValue_B` |
| Final | `CalibrationValue = (A + B) / 2` — averages out TX/RX path asymmetry |

Write the result to production firmware:
```cpp
radio.setRangingCalibration(YOUR_AVERAGED_VALUE);
```

### Known results (2026-07-02)

| Run | CalibrationValue |
|---|---|
| A (master=board1) | −35 |
| B (master=board2) | _pending_ |
| **Final** | **(−35 + B) / 2** |

---

## Implementation Notes

- RadioLib does **not** map DIO1 to ranging IRQ events — firmware uses a 300 ms polling wait instead of ISR
- Uncalibrated ranging returns ~−5 to −7 m (chip internal delays); this is normal and corrected by calibration
- `getRangingResult()` returns metres; calibration formula converts to raw counts for `setRangingCalibration()`

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

Confirmed from schematic + community references. Earlier versions of this file incorrectly listed DIO1=36/BUSY=34.

| Signal | GPIO |
|---|---|
| CS / NSS | 7 |
| SCK | 5 |
| MOSI | 6 |
| MISO | 3 |
| DIO1 / IRQ | **9** |
| BUSY | **36** |
| RESET | 8 |

---

## Build Environment

Verified on macOS (Apple Silicon) and Arch Linux x86_64.

| Component | Version |
|---|---|
| PlatformIO | 6.1.19+ |
| RadioLib | 7.7.1 |
| espressif32 platform | 7.0.1 |
| Framework | Arduino |

| Environment | Status | RAM | Flash |
|---|---|---|---|
| master | ✅ SUCCESS | 6.1% | 8.8% |
| slave | ✅ SUCCESS | 6.1% | 8.8% |
