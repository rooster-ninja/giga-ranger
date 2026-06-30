# SX1280 Ranging Calibration Setup
### RF Attenuator & Reference Cable for Conducted Calibration

> **Devices:** Semtech SX1280 @ 2.4 GHz — LILYGO T3-S3 (ESP32-S3) platform  
> **Goal:** Establish a conducted calibration reference to derive the `RxTxDelay` register offset for a 60 km fixed point-to-point ranging link in British Columbia

---

## Background

The SX1280 ranging engine measures distance via round-trip time-of-flight between a Master and Slave radio. Before deploying over a long link, a **conducted calibration** must be performed to characterise and subtract fixed hardware delays (PCB traces, TX/RX switching, front-end group delay) that would otherwise bias every distance measurement.

Semtech documents this procedure in:

- **AN1200.29** — *Introduction to Ranging with the SX1280*  
  [https://www.semtech.com/uploads/documents/introduction_to_ranging_sx1280.pdf](https://www.semtech.com/uploads/documents/introduction_to_ranging_sx1280.pdf)

- **SX1280 Datasheet Rev 3.2** — Section 14.5 (Ranging Engine) and Table 14-60 (`CalibrationValue` register)  
  [https://media.digikey.com/pdf/Data%20Sheets/Semtech%20PDFs/SX1280-81_Rev3.2_Mar2020.pdf](https://media.digikey.com/pdf/Data%20Sheets/Semtech%20PDFs/SX1280-81_Rev3.2_Mar2020.pdf)

The calibration value is **SF-dependent** and **board-design-dependent** — it must be measured empirically for each hardware design and written to the `RxTxDelay` register. Once set, the SX1280 applies the correction automatically to all subsequent ranging results.

---

## Calibration Method (per AN1200.29)

The conducted setup connects the two radios through a signal chain of **known electrical length**, with sufficient attenuation to prevent receiver saturation:

```
[SX1280 Master]──SMA──[Atten]──[1 m coax]──[Atten]──SMA──[SX1280 Slave]
```

1. Average a large number of ranging exchanges (role-reversed Master↔Slave to cancel asymmetric delays)
2. The averaged `RangingResult` represents the total round-trip delay through the hardware *plus* the known cable electrical length
3. Subtract the known cable contribution (`D_cable`) to isolate the hardware offset
4. Write the result to the `RxTxDelay` register

Per the ARM Mbed SX1280 driver (sourced from Semtech reference code):

> *"The calibration value reflects the group delay of the radio front end and must be re-performed for each new SX1280 PCB design. The values are Spreading Factor dependent."*

---

## Attenuation Requirements

| Parameter | Value |
|---|---|
| SX1280 max TX power | +12.5 dBm |
| SX1280 RX sensitivity (SF10, BW=406 kHz) | −122 dBm |
| Max coupling loss budget | ~134.5 dB |
| Recommended conducted attenuation | **80 dB total** |
| Cable insertion loss (1 m, typical) | ~0.1–0.3 dB |

80 dB keeps the receiver well out of saturation and AGC non-linearity while maintaining a strong enough signal for reliable ranging exchanges. AN1200.29 also recommends placing **both radios in shielded metal enclosures** to eliminate any radiated leakage path that would corrupt the conducted reference measurement.

---

## Recommended Hardware

### Attenuators — 2× 40 dB SMA Inline (in series = 80 dB total)

| Part | Vendor | Freq | Power | Body | Notes |
|---|---|---|---|---|---|
| **PE7001-40** | Pasternack | DC–3 GHz | 2 W | Stainless | Lowest cost; adequate for 2.4 GHz |
| **PE7466-40** | Pasternack | DC–12 GHz | 2 W | Stainless | Better frequency headroom |
| **SA18L-40** | Fairview Microwave | DC–18 GHz | 2 W | Stainless | 1.35 VSWR; SMA M→F |

At 12.5 dBm TX power (~18 mW), any 2 W-rated attenuator is more than adequate. The PE7001-40 (DC–3 GHz) is the practical choice — in-stock at Pasternack, same-day shipping, and 2.4 GHz sits comfortably within its specification.

All are **SMA male plug → SMA female jack** inline bodies. Chain them in series; no soldering required for the attenuators themselves.

---

### Reference Cable — 1 m Coax with SMA Male Connectors on Each End

A 1 m cable provides a well-defined, non-trivial electrical length that:

- Falls well within the 80 dB link budget
- Produces a ranging result clearly offset from zero (avoiding edge-case behaviour in the raw result register)
- Has a precisely calculable electrical length once the cable's **velocity factor (VF)** is known

#### Velocity Factor by Cable Type

| Cable | VF | Electrical length of 1 m physical |
|---|---|---|
| RG-316 | 0.695 | 0.695 m |
| RG-174 | 0.66 | 0.66 m |
| LMR-100 | 0.80 | 0.80 m |
| RG-58 | 0.66 | 0.66 m |
| Semi-rigid UT-141 | 0.695 | 0.695 m |

The VF is printed on the cable jacket or in the manufacturer datasheet. Use:

```
D_cable (m) = physical_length (m) × velocity_factor
```

#### SMA Connectors for Solder Termination

Use **SMA straight solder plugs (male)** matched to your cable's outer diameter:

| Cable | Connector (Amphenol) | Available from |
|---|---|---|
| RG-316 | 132289 series | Mouser, DigiKey |
| RG-174 | 901-143J series | Mouser, DigiKey |

Both are standard, inexpensive parts with solder centre-pin and crimp/clamp outer contact suitable for bench assembly.

---

## Complete Signal Chain

```
┌─────────────────┐     ┌──────────┐   ┌──────────┐
│  T3-S3 Master   │     │  40 dB   │   │  40 dB   │
│  (metal box)    │─SMA─│  Atten   │───│  Atten   │───┐
└─────────────────┘     │ PE7001-40│   │ PE7001-40│   │
                        └──────────┘   └──────────┘   │
                                                       │ 1 m coax
                                                       │ SMA-M / SMA-M
                                                       │
┌─────────────────┐                                    │
│  T3-S3 Slave    │────────────────────────────────────┘
│  (metal box)    │─SMA
└─────────────────┘
```

Total insertion: **80 dB attenuation + known cable electrical length (D_cable)**

---

## Applying the Calibration

Once the averaged `RangingResult` is obtained from the conducted setup:

1. Convert `D_cable` to ranging result units:  
   `RangingResult_cable = D_cable / 0.1803`  
   *(0.1803 m/count — empirically derived from SX1280 field measurements; verify against your own known-distance radiated test)*

2. `CalibrationValue = RangingResult_measured − RangingResult_cable`

3. Write `CalibrationValue` to the `RxTxDelay` register for each SF in use

4. Repeat at the operating temperature — crystal oscillator drift shifts the calibration value with temperature

> The `RxTxDelay` register address and bit format are defined in **SX1280 Datasheet Rev 3.2, Table 14-60**.

---

## Key References

| Document | Description | Link |
|---|---|---|
| AN1200.29 | Introduction to Ranging — SX1280 (Semtech) | [semtech.com](https://www.semtech.com/uploads/documents/introduction_to_ranging_sx1280.pdf) |
| DS.SX1280-1 Rev 3.2 | SX1280 Datasheet — Section 14.5, Table 14-60 | [digikey.com (PDF)](https://media.digikey.com/pdf/Data%20Sheets/Semtech%20PDFs/SX1280-81_Rev3.2_Mar2020.pdf) |
| StuartsProjects field test | 40 km SX1280 ranging — empirical conversion factor | [stuartsprojects.github.io](https://stuartsprojects.github.io/2019/04/26/Semtech-SX1280-2-4Ghz-LoRa-ranging-tranceivers.html) |
| Pasternack PE7001-40 | 40 dB SMA attenuator, DC–3 GHz, 2 W | [pasternack.com](https://www.pasternack.com/40db-fixed-sma-female-sma-male-2-watts-attenuator-pe7001-40-p.aspx) |
| Fairview SA18L-40 | 40 dB SMA attenuator, DC–18 GHz, 2 W | [fairviewmicrowave.com](https://www.fairviewmicrowave.com/product/40db-fixed-attenuator-sma-male-sma-female-2-watts-sa18l-40.html) |

---

*Last updated: June 2026*
