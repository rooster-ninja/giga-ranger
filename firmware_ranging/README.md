<p align="center">
  <img src="../Assets/GooseLogo.png" alt="Goose" width="320"/>
</p>

<h1 align="center">Giga Ranger — Production Ranging Firmware</h1>
<p align="center"><em>SX1280 continuous ranging for LILYGO T3-S3 V1.3</em></p>

---

## Overview

Production firmware for the 60 km fixed LOS ranging link between **Alpha** (permanent master)
and **Chimp-001** (permanent slave). No WiFi. No MQTT. Each device monitors locally via BLE
terminal and an onboard OLED display.

- **Alpha** initiates ranging every 5 seconds, displays distance + signal quality
- **Chimp-001** responds to ranging requests, displays signal quality and exchange count
- Both devices advertise a BLE NUS (Nordic UART Service) terminal — connect from any phone

---

## Flash Instructions

**1. Flash Chimp-001 first**
```bash
cd firmware_ranging
pio run -e chimp-001 -t upload --upload-port /dev/cu.usbmodem<CHIMP001>
```

**2. Flash Alpha**
```bash
pio run -e alpha -t upload --upload-port /dev/cu.usbmodem<ALPHA>
```

**3. Serial monitor (USB)**
```bash
pio device monitor --port /dev/cu.usbmodem<ALPHA> --baud 115200
```

**Tip — find ports:** `ls /dev/cu.*`

---

## BLE Monitoring

Connect from any phone using a BLE UART app. Recommended apps:

| App | Platform | Notes |
|---|---|---|
| nRF UART v2.0 | iOS + Android | Nordic official |
| Serial Bluetooth Terminal | Android | Simple, reliable |
| nRF Toolbox | iOS + Android | Multi-tool |

Advertisement names:
- **Alpha:** `GR-ALPHA`
- **Chimp-001:** `GR-CHIMP001`

### BLE output format

**Alpha** (one line per exchange):
```
ALPHA,t=1234,r=60234.1,rssi=-82,snr=12.3,ok=1234,rej=2
```

**Chimp-001** (one line per exchange):
```
CHIMP,t=1234,rssi=-84,snr=11.8,ok=1234
```

`t` = seconds since boot. `r` = rolling median range in metres. `rej` = outlier count.

---

## Display Layout

Board mounted vertically — display rotated 90° (portrait, 64×128 px effective).

**Alpha:**
```
ALPHA
BLE:CONN
──────────
60234.1 m
RSSI:-82dBm
SNR: 12.3dB
──────────
OK:   1234
DIE: 31.2C
```

**Chimp-001** (slave does not compute distance):
```
CHIMP-001
BLE:WAIT
──────────
RSSI:-84dBm
SNR: 11.8dB
──────────
OK:   1234
DIE: 28.5C
```

---

## Pin Assignments (T3-S3 V1.3)

| Signal | GPIO | Notes |
|---|---|---|
| SX1280 NSS | 7 | |
| SX1280 SCK | 5 | |
| SX1280 MOSI | 6 | |
| SX1280 MISO | 3 | |
| SX1280 DIO1 | 9 | |
| SX1280 BUSY | 36 | |
| SX1280 RESET | 8 | |
| OLED SDA | 17 | I2C shared with BME280 |
| OLED SCL | 18 | I2C shared with BME280 |
| BME280 SDA | 17 | Address 0x76 (SDO→GND) |
| BME280 SCL | 18 | |

OLED I2C address: `0x3C`. Display: SSD1306 128×64.

---

## Outlier Filter

Two-stage filter on Alpha's range readings:

1. **Delta gate** — rejects any reading deviating > ±500 m from the last valid reading.
   Catches SX1280 hardware glitches that produce out-of-range single-sample results.
   Threshold is defined as `DELTA_GATE_M` in `src/main.cpp` — tighten after observing
   natural field σ (suggestion: set to ~10× observed σ).

2. **Rolling median (N=5)** — published range is the median of the last 5 accepted readings.
   Naturally resistant to remaining outliers; smooths residual noise.

---

## RF Parameters

| Parameter | Value |
|---|---|
| Frequency | 2450 MHz |
| Bandwidth | 1625 kHz |
| Spreading Factor | SF9 |
| TX power | 13 dBm OTA (PA FEM — never exceed +5 dBm conducted) |
| Ranging address | 0xDEADBEEF |
| Calibration table SF9 | CAL_TABLE[2][4] = 13089 |
| Cal date | 2026-07-04, die temp 31.3°C |

See `firmware_calibration/README.md` for full calibration history.

---

## Build Environment

| Component | Version |
|---|---|
| PlatformIO | 6.1.19+ |
| RadioLib | 7.7.1 |
| U8g2 | 2.35+ |
| NimBLE-Arduino | 2.1.3+ |
| Adafruit BME280 | 2.2.4+ |
| espressif32 platform | 7.0.1 |
| Framework | Arduino |
