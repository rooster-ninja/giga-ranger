# SX1280 Calibration Debug Summary

## ⚠️ Board Rename (2026-07-16)
Roles permanently swapped. Old "Chimp" is now **Alpha** (master). Old "Alpha" is now **Chimp** (slave).
ACM port assignments stay the same — only the names changed.

| New name | Old name | Role       | ACM port | CAL_TABLE[2][4] |
|----------|----------|------------|----------|-----------------|
| Alpha    | Chimp    | Master     | ACM2     | **13343**       |
| Chimp    | Alpha    | Slave      | ACM1     | 13115 (as master, for reference only) |

**Production cal (Alpha as master): 13343** ← CURRENT (RadioLib 7.7.1)
**AN1200.89 average (both boards as master): 13344** (13343 + 13346) / 2 = 13344.5

## Calibration Results Summary (auto_cal, n=500, RadioLib 7.7.1)

| Master | CAL_TABLE[2][4] | mean (m) | sigma (m) | die (°C) | amb (°C) | filter | date |
|--------|-----------------|----------|-----------|----------|----------|--------|------|
| Alpha  | **13343**       | 0.6849   | 0.9813    | 38.6     | 27.1     | none   | 2026-07-16 |
| Chimp  | **13346**       | 0.7050   | 0.9581    | 39.6     | 27.7     | none   | 2026-07-17 |
| _avg_  | **13344**       | —        | —         | —        | —        | AN1200.89 avg | — |
| Alpha  | **13316**       | 0.7032   | 0.5708    | ~36      | 25.5     | IQR×3  | 2026-07-17 |
| Alpha  | 13229           | 0.7187   | 0.5961    | 38.6     | —        | none   | superseded — RadioLib <7.7.1 |

**AN1200.89 note:** (13343 + 13346) / 2 = 13344.5 → **13344**. The 3-count spread is within
measurement noise (σ/√500 ≈ 45mm → ~2 counts), so both boards are functionally identical.

**⚠️ CAL=13316 is NOT the definitive production value.** It converged at a cooler bench temperature
(die ~36°C, amb 25.5°C) vs the July 16 sessions (die 38-39°C, amb 27-28°C). A 0.75m systematic
shift was observed at the SAME cal value (13343) between sessions (see thermal drift section below).
A dedicated temperature regression sweep is required before any fixed CAL value can be trusted for
outdoor production use.

**Production firmware** currently uses `CAL_TABLE[2][4] = 13343` (Alpha session, July 16).

**RadioLib version note:** Upgrading from <7.7.1 to 7.7.1 shifted all SF9/BW1625 readings by +2.57m
(114 counts × 0.02253 m/count). Re-calibration is required after any RadioLib version change.

**Oscillation in Chimp calibration (iterations 3-7):** Both cal=13323 and cal=13325 produced
means ranging from 0.64m to 1.17m across different 500-sample runs. Root cause: measurement σ ≈ 1m
gives σ_mean ≈ 45mm, which exceeds the ±30mm tolerance. Near the calibration point, individual runs
can land either side of target. Auto_cal recovered by interpolating the history and jumping to 13346.

---

## ⚠️ Critical: Session-to-Session Thermal Drift (2026-07-17)

A new IQR-filtered auto_cal run (Alpha as master, Arch laptop, ACM1) started at CAL=13343 and found
a mean of **−0.066m** in iteration 1 — compared to **+0.685m** measured at the same CAL on July 16.
That is a **0.75m systematic shift** at identical hardware configuration.

### Session comparison at CAL=13343

| | Session 1 (2026-07-16) | Session 2 (2026-07-17) | Δ |
|---|---|---|---|
| CAL | 13343 | 13343 | 0 |
| mean (m) | +0.6849 | −0.0655 | **−0.750 m** |
| sigma raw (m) | 0.9813 | 1.4348 | — |
| sigma filtered (m) | — | 0.5562 | — |
| die (°C) | 38.6 | ~36 | **−2.6°C** |
| amb (°C) | 27.1 | 25.5 | **−1.6°C** |

The −0.750m shift is ~30 standard errors (σ_mean ≈ 0.025m). It is definitively real.

### Implied temperature coefficient

| Variable | Δ | Implied coefficient |
|----------|---|---------------------|
| Die temp | −2.6°C | **+0.29 m/°C** (lower die → lower reading) |
| Ambient  | −1.6°C | **+0.47 m/°C** |

**Sign conflict with icebox sweep:** The icebox sweep (2026-07-16) found −0.063 m/°C
(higher die → lower reading). This session-to-session comparison implies the opposite direction
and a 4-8× larger magnitude. The root cause is unresolved — candidates:
- Two-session confound: different cable seating, connector contact between sessions
- Ambient-temperature dependence of crystal oscillator (SX1280 ranging timer)
- Cable thermal expansion (~0.0002 m/°C for 1m RG-316 is negligible; rules this out)
- The icebox sweep was done at CAL=13229 with CPU burn active, not IQR-filtered — possible confound

### New convergence at lower temperature

After −0.066m at iteration 1, auto_cal converged in 9 iterations at **CAL=13316**
(mean=0.7032m, σ=0.5708m, die ~35–36°C, amb 25.5°C).
CAL shift: 13343 − 13316 = **27 counts = 0.608m** at 0.02253 m/count.

### IQR filter validated across all 9 iterations

| Iter | CAL | σ_raw (m) | σ_filtered (m) | rejected / ~500 |
|------|-----|-----------|----------------|-----------------|
| 1 | 13343 | 1.4348 | 0.5562 | 2 |
| 2 | — | 0.6504 | 0.5377 | 1 |
| 3 | — | 2.6486 | 0.5418 | 2 (incl. −47.6m outlier) |
| 7 | — | 2.5039 | 0.5293 | 4 |
| 9 (final) | 13316 | 1.3445 | 0.5708 | 4 |

Consistent post-filter σ of 0.53–0.57m confirms the filter is correct. The raw σ variance
(0.5–2.6m) reflects rare SX1280 stale-register reads, now cleanly removed.

### Next required step: Dedicated thermal regression sweep

**Before any fixed CAL value can be trusted for production, characterize mean vs die_temp directly:**
1. Bench rig: Alpha (master) ── PE7601-40 attenuator ── 1m RG-316 ── Chimp (slave)
2. Tool: IQR-filtered auto_cal.py — but repurposed to hold CAL fixed and log (die_temp, mean)
3. Sweep die temp from min achievable bench temp to max (≥30°C span): icepack, ambient, hot-air gun
4. Record (die_temp, amb_temp, filtered_mean) at each stable plateau
5. Fit linear regression: mean = A × die_temp + B
6. Implement in firmware_ranging: `range_corr = raw_range − A × (die_temp − cal_die_temp)`
7. Recheck with temperature compensation in place

---

## ⚠️ Critical: SNR-Dependent Ranging Bias (2026-07-02)

### Background

A "for giggles" auto_cal run was conducted with **two** PE7601-40 attenuators in series (80 dB total)
to characterize how the second attenuator's electrical length registers in CAL_TABLE[2][4].

**Bench rig:** Alpha ── 40 dB atten ── 40 dB atten ── 1 m RG-316 ── Chimp  
**Expected signal at receiver:** +13 dBm − 80 dB ≈ **−67 dBm** (link margin ~35 dB above sensitivity)  
**Bench rig at normal 40 dB:** +13 dBm − 40 dB ≈ **−27 dBm** (margin ~75 dB)

### What happened

The script ran 15 iterations and **never converged**. CAL oscillated between 13608–13635, with
the mean locked ~0.22m below the 0.695m target at all tried values:

| Setup | Approx. converged CAL | Notes |
|-------|----------------------|-------|
| 40 dB (single atten) | **13316–13346** | Stable, reproducible across sessions |
| 80 dB (dual atten, this run) | **~13612** (never stable) | +~296 counts from 40 dB baseline |

**~296 counts = ~6.7 m equivalent electrical path** — grossly larger than the physical length
of any SMA attenuator (~20–30 mm). The second attenuator's *physical* electrical contribution
is < 0.001 m. The ~6.7 m is not cable length — it is **SNR-dependent ranging bias**.

### Run-to-run variance at fixed CAL

At 40 dB, repeated runs at the same CAL show mean variance of ~0.1 m (explained by σ_mean ≈ 0.022 m
per batch, with some thermal drift). At 80 dB, the same fixed-CAL runs showed:

| CAL | Observed means across iterations |
|-----|----------------------------------|
| 13622 | 0.4145, 0.5296, 0.4798 m — spread: **0.37 m** |
| 13621 | 0.4312, 0.5098, 0.4310 m — spread: **0.18 m** |

Run-to-run mean variance at the same CAL and same temperature is ~4× larger at 80 dB than at 40 dB.
This is not just noisier individual samples (σ_filtered remained ~0.53–0.62 m, similar to before) —
the **batch mean itself** is unstable. The SX1280 ranging correlator is intermittently locking onto
different points in the correlation leading edge depending on noise realization.

### Root cause: SX1280 ranging timing bias is SNR-dependent

The SX1280 ranging uses a leading-edge correlation timing detection. At high SNR the correlation
peak is sharp and timing is precise. As SNR drops:
1. The correlation peak broadens and the leading edge flattens
2. The timing bias of the peak detector shifts — lower SNR → later apparent detection → longer
   apparent ToF → higher apparent range
3. Small SNR fluctuations (thermal noise, minor gain variation) can tip between "detection modes"

The CAL_TABLE in AN1200.89 corrects for the **fixed** internal processing delay at a **specific SNR
operating point**. It does NOT generalize across SNR regimes. Each SNR regime has a different
effective zero-point.

### Implication for the 60 km production link

| Setup | RSSI | Link margin above floor | CAL regime |
|-------|------|------------------------|------------|
| Bench 40 dB atten | ~−27 dBm | ~75 dB | "High SNR" |
| Bench 80 dB atten | ~−67 dBm | ~35 dB | "Mid SNR" — oscillated, never converged |
| **60 km field link** | **~−105 dBm** | **~3 dB** | **"Near-floor" — completely uncharacterized** |

The field link operates at an SNR regime nowhere near the bench calibration point.
A fixed CAL_TABLE value calibrated on the bench will have an unknown systematic offset at 60 km.

### Consequences

1. **No single bench-derived CAL value transfers reliably to the 60 km field link.**
   The bias shifts with SNR; the field SNR is far outside the bench characterization range.

2. **Linear thermal compensation (firmware_ranging `TEMP_COEFF`) does not fix this.**
   SNR-dependent bias is a separate effect that compounds with thermal effects.

3. **In-situ calibration using Board 3 is not just helpful — it is likely necessary for
   any sub-meter production accuracy.** A co-located Board 3 at Alpha's site (known distance ≈ 0 m)
   corrects thermal hysteresis, SNR-dependent bias, connector state, and long-term drift
   simultaneously, without needing to model any of them independently.

4. **For field characterization:** The BLE NUS output already includes die_c. Adding RSSI/SNR to
   each line would allow direct characterization of ranging bias vs signal level across the
   actual link geometry during initial deployment — this is more informative than any bench test.

### Final run data: complete 15-iteration summary (2026-07-17)

After the run completed at [FAIL], the collected history was:

| CAL | mean (m) | Notes |
|-----|----------|-------|
| 13635 | +0.039 | early iteration, far above true answer |
| 13632 | +0.002 | |
| 13632 | +0.773 | **same CAL, 0.77m different mean** |
| 13621 | +0.212 | |
| 13618 | +0.272 | |
| 13617 | +0.279 | |
| 13616 | +0.432 | |
| 13616 | **+1.045** | **same CAL, 0.61m different mean** |
| 13611 | +0.412 | |
| 13610 | +0.460 | |
| 13610 | +0.593 | |
| 13609 | +0.626 | |
| 13609 | +0.657 | |
| 13609 | **+0.912** | **same CAL, 0.29m from previous run at 13609** |
| 13608 | +0.875 | |

Three runs at CAL=13609 produced means of 0.626, 0.657, 0.912m (spread = 0.286m).  
Expected σ_mean from noise (500 samples, σ≈0.58m): **0.026m**.  
Actual batch-to-batch spread: **0.15–0.61m — 6–23× larger than expected from noise alone.**

The batch mean is fundamentally unstable at this attenuation level. The convergence zone is approximately **CAL ≈ 13609–13610** (both sides of target appear), but no reliable single value can be extracted.

**Summary of second-attenuator result:** The 80 dB setup requires approximately 293 counts more CAL than single-attenuator (~13316 → ~13609). Electrical path interpretation: **6.60m equivalent** — which is dominated by SNR-dependent timing bias, not physical cable length.

### Second attenuator electrical path (as measured by SX1280)

For completeness: the PE7601-40's "electrical length" as seen by the SX1280 ranging calibration
at 40 dB working point is **~6.7 m** (296 counts × 0.02253 m/count). This is an artifact of
SNR-dependent timing shift, not a physical path length. The number is meaningless for production
use; it is documented here to explain the result, not to be trusted as a calibration constant.

---

## ⚠️ Critical: AGC Discrete Gain-State Switching (2026-07-19)

### Observation

During an 80 dB bench sweep (CAL=13382, Alpha master, ACM2), the ranging mean and RSSI register
jumped simultaneously between batch 14 and batch 15 with no change to hardware or temperature:

| Batch | die_c | RSSI (dBm) | mean_m | σ (m) | rej |
|-------|-------|-----------|--------|-------|-----|
| 12 | 39.6 | −30.6 | +1.559 | 0.680 | 2 |
| 13 | 39.6 | −30.8 | +1.565 | 0.554 | 2 |
| **14** | **38.6** | **−31.1** | **+1.330** | **0.703** | **42** |
| **15** | **38.6** | **−41.7** | **+4.433** | **0.649** | **1** |
| 16 | 38.6 | −41.8 | +4.419 | 0.633 | 2 |
| 17 | 38.6 | −41.8 | +4.813 | 0.647 | 1 |

Three things changed simultaneously at a single batch boundary:
- **RSSI**: −31.1 → −41.7 dBm (clean +10.6 dBm step)
- **Mean**: +1.33 → +4.43 m (+3.10 m jump)
- **Rejects**: 42 → 1 (back to baseline)

Temperature at the boundary (38.6°C) was identical on both sides. Setup unchanged.

### Two discrete operating states

The system oscillates between two mutually exclusive lock modes across the full 19-batch run:

| State | RSSI | Mean | σ | Rej/batch | Batches |
|-------|------|------|---|-----------|---------|
| A (wrong lock) | −41.6 to −41.8 dBm | 4.4–5.1 m | ~0.63 m | 0–2 | 1–6, 15–19 |
| B (correct lock) | −30.6 to −32.2 dBm | 1.3–5.2 m (falling) | ~0.65 m | 0–2 (42 at transition) | 7–14 |

Within each state, σ ≈ 0.6 m and rejects are near-zero — single-cluster distributions, not
mixed. The 42 rejects in batch 14 were the mid-batch B→A transition: samples from both states
within one 500-sample window, producing a bimodal distribution that the IQR filter partially
separated. The mean was falling within State B (AGC settling after the A→B transition at
batch 6→7). State A is stable at ~4.5–5 m. State B converges toward ~1.3 m then snaps back.

### Hypothesis: discrete AGC gain-state transition

The SX1280 AGC selects from a set of discrete LNA gain steps (Table 4-2: 1–13). At the 80 dB
bench operating point (~−67 dBm received), the signal sits near an AGC threshold. When the
AGC switches between two adjacent gain steps:

1. **Amplitude change:** Different gain → different received amplitude → different REG_RANGING_RSSI
   value. The +10.6 dBm RSSI step matches a plausible ~2 gain-step transition.

2. **Group delay change:** Each gain step has a different propagation delay through the RF front
   end. A gain-step switch shifts the effective signal arrival time seen by the ranging correlator,
   directly shifting the measured ToF → +3.1 m jump in apparent range at 0.02253 m/count.

This also explains the two 80 dB sessions disagreeing (−38.5 dBm in session 1 vs −31 dBm in
session 2 and State B of session 3): each session locked into a different AGC gain state near
startup and stayed there — internally consistent per session, not comparable across sessions.

**SX1280 gain control architecture (datasheet Section 4.2):**
- Two LNA regimes: Low Power Mode (default, AGC capped below top 3 gain steps) and
  High Sensitivity Mode (register 0x0891 bits 7:6 = 0x3, unlocks top 3 steps, +3 dB NF)
- Manual gain: three register writes per Table 4-1 —
  `0x089F bit 7 = 1`, `0x0895 bit 0 = 1`, `0x089E bits 3:0 = step (1–13)`
- No documented readback of "current AGC gain step" in driver headers, but `0x089E` is
  readable and returns the current step value when AGC is active

### Test: gain step readback (2026-07-19)

**Firmware change (commit 054c20e):** `read_radio_state()` now reads both REG_RANGING_RSSI
(0x0964) and REG_GAIN_VALUE (0x089E bits 3:0) in one STANDBY_XOSC window after each exchange.
CSV gains a `gain_step` column. Stats line reports mode gain step per 500-sample batch.

**`FIXED_GAIN` define added:** Set to 0 (AGC auto, default) or 1–13 to lock a specific step.
Use 0 first to characterize which steps AGC selects at each attenuation level, then lock
once the correct step per attenuation regime is identified.

### Test results (2026-07-19, commits 054c20e / 27c44c6)

**`FIXED_GAIN=10`** (4 batches, AGC State A conditions):
- gain_step=10 confirmed on every sample
- RSSI=−41.3 dBm, mean=5.40 m, batch-to-batch variance=**0.04 m** (≈1.5× noise floor)
- Perfectly stable. Fixed gain eliminates the random A↔B transitions entirely.

**`FIXED_GAIN=8`** (3 batches):
- gain_step=8 confirmed on every sample
- RSSI=−40.4 dBm, mean=4.36 m — State B did **not** appear.

| Effect of 2 gain steps (10→8) | Value |
|-------------------------------|-------|
| RSSI shift | −0.9 dBm (0.45 dBm/step) |
| Mean shift | −1.05 m (0.52 m/step) |

### Hypothesis refuted — revised understanding

**State B is not reachable by gain adjustment.** State B's RSSI signature (−31 dBm) is 9.4 dBm
less negative than gain=8 (−40.4 dBm). At 0.45 dBm/step, reaching it would need ~21 more
steps — exceeding the 13-step hardware range by 8. The AGC gain-step hypothesis is **wrong**
for the A/B state distinction.

**Revised: States A and B are different correlator lock decisions, not gain steps.**

- **State A** (~5 m, RSSI ~−41 dBm): correlator locks on a spurious sidelobe with
  *higher* correlation amplitude. Stable at gain=10 and gain=8.
- **State B** (~1.3 m, RSSI ~−31 dBm): correlator locks on the *true* ToF peak, which has
  *lower* correlation amplitude. Occurred stochastically in free-running AGC; not
  reproducible by setting a lower fixed gain.

The 10.6 dBm RSSI jump between states reflects the amplitude difference between correlation
peaks (spurious sidelobe > true ToF peak at this SNR), not a gain-step change.

### What manual gain control IS useful for

1. **Stability:** Fixed gain eliminates random A↔B transitions. Gain=10 locked achieved
   0.04 m batch-to-batch variance vs. multi-metre jumps in AGC mode.
2. **Group delay characterisation:** The 0.52 m/step coefficient is a real secondary
   systematic. If the 40 dB calibration (higher signal → AGC picks a lower gain step)
   uses a different step than the 80 dB measurement, the group delay difference
   compounds the SNR-dependent bias. **Open experiment:** run a few batches at 40 dB
   with `FIXED_GAIN=0` and observe which gain step AGC naturally picks.

### Open question: what is the spurious ~5 m lock target?

At gain=10, mean ≈ 5.4 m with CAL=13382 → offset = 5.4 − 0.695 ≈ 4.7 m ≈ 208 ranging counts.
This does not correspond to an obvious multiple of SF9 chip period or LoRa symbol length. Possible
causes: multipath within the coaxial bench rig, a known SX1280 correlation function alias, or
an undocumented leading-edge detection artifact at low SNR.
**Proposed test:** change cable length (e.g., 0.5 m cable) and see if State A mean shifts by
the same 0.15 m — if it tracks, the spurious lock is relative to the cable. If it doesn't move,
it's a fixed correlator artifact.

---

## REG_RANGING_RSSI (0x0964) — Convention Note (2026-07-19)

**For web publication:** The formula and convention for this register are non-obvious and worth documenting.

### Register and formula

The SX1280 ranging engine exposes the RSSI of the last ranging exchange (slave's response as received
by the master) at register **0x0964 (REG_RANGING_RSSI)**. Formula from the SX1280 Programming Guide
register map:

```
RSSI_dBm = -(float)rssi_raw / 2.0f
```

**Source:** SX1280/SX1281 Data Sheet / SX1280 Programming Guide, register 0x0964. AN1200.89
(Theory and Principle of Advanced Ranging) is a theory/principles document and does not document
individual registers. The formula is consistent with Semtech's reference implementation in the
SX1280 SDK and in RadioLib's SX128x driver.

### The inverted-convention trap

In **standard dBm**, less negative = stronger signal (e.g., −27 dBm is stronger than −67 dBm).

REG_RANGING_RSSI **reverses this**:

| Bench attenuation | Expected received power | rssi_raw | Register output | Direction |
|-------------------|------------------------|----------|-----------------|-----------|
| 40 dB | ~−27 dBm (strong) | ~126 | **−63 dBm** | more negative = stronger |
| 80 dB run 1 | ~−67 dBm (weak) | ~77 | **−38.5 dBm** | less negative = weaker |

**More negative register output = stronger received signal.** This is the opposite of standard RSSI
convention.

### Why this happens

The formula `-rssi_raw/2` follows Semtech's convention where rssi_raw encodes signal amplitude
(larger raw = stronger signal → formula gives more negative dBm output). The ranging RSSI
register appears to measure the correlation peak amplitude after despreading rather than raw
RF input power — the processing gain of SF9 (~27 dB) is visible in the values
(-67 dBm received → ~-38.5 dBm register output, Δ≈28.5 dBm ≈ SF9 gain). At 40 dB (strong
signal), the AGC reduces gain to prevent saturation, which is why the register reads more
negative than the raw Δ formula would predict.

### Practical upshot

The register value is **monotonically useful** as a relative signal-strength indicator for
the SNR-dependent bias curve: more negative = higher SNR = lower expected ranging bias.
It is NOT a calibrated absolute RSSI. Use it for:
- Correlating ranging bias to received signal level
- Detecting whether bench SNR conditions have changed between runs
- Flagging low-SNR exchanges in production BLE NUS output

**Do not interpret the absolute dBm value as true received power.**

### Measured reference points (firmware_calibration, SF9/BW1625, Alpha as master)

| Setup | CAL | rssi_raw (approx.) | Register output | Notes |
|-------|-----|--------------------|-----------------|-------|
| 40 dB bench | 13382 | ~126 | **−63 dBm** | Reference calibration point |
| 80 dB bench, run 1 | 13316 | ~77 | **−38.5 dBm** | First dual-attenuator run |
| 80 dB bench, run 2 | 13382 | ~62–64 | **−31 to −32 dBm** | Second run, conditions differ |

---

## Temperature Sweep Analysis (2026-07-16)

Contiguous sweep file: `Assets/master_20260716_214604.csv`
Firmware: calibration master, CAL=13229 (RadioLib 7.7.1), CPU burn active.
Duration: 141 min, 11,709 raw rows.

### Phase summary

| Phase | Die (°C) | raw_m median | σ (m) | n |
|-------|----------|-------------|-------|---|
| Ambient start | 39.6 | 3.200 | 0.80 | 375 |
| Cold stable   | 16.6 | 4.642 | 0.99 | 625 |
| Hot stable    | 48.6 | 2.637 | 0.79 | 897 |
| Ambient end   | 39.6 | 2.885 | 0.69 | 416 |

All absolute readings offset by +2.505m from calibrated zero because sweep used CAL=13229;
corrected (subtracting offset) values would be: cold=+2.14m, hot=−0.56m vs 0.695m target.

### Temperature coefficient

| Metric | Value |
|--------|-------|
| Phase slope (cold → hot) | **−0.063 m/°C** die temp |
| In cal-count units | **−2.8 counts/°C** |
| Δdie measured | 32.0°C (16.6 → 48.6°C) |
| Δraw measured | −2.01 m |
| Per 10°C die change | ~0.63 m reading shift |

**Direction: higher die temperature → lower ranging reading.**

Practical impact: a ±15°C die excursion from calibration point shifts the reading by ~±0.95m.
For the 60 km production link, individual-reading σ ≈ 1–2m, so temperature-induced drift is
within the noise floor. Long-run averaged measurements benefit from knowing the die temperature
(already logged in BLE NUS output).

Long-term drift at constant die: −0.32m over 2.4h (within measurement uncertainty at n≈400,
σ_mean ≈ 0.04m → ~8σ; likely thermal equilibration of board structure, not oscillator aging).

---

---

---

## Goal

Find the correct `CAL_TABLE[2][4]` value for a LILYGO T3-S3 V1.3 board (Chimp) running as
SX1280 ranging **master**, so that the mean measured distance equals **0.695 m** — the
electrical length of the calibration cable (1 m RG-316, VF=0.695).

Bench setup: Chimp (ACM2, master) ── 40 dB attenuator ── 1 m RG-316 cable ── Alpha (ACM1, slave)

RadioLib formula: `distance = (rawToF − calibration) × 150 / (4096 × BW_kHz)`
For SF9/BW1625: 1 cal count ≈ 0.02253 m change (theoretical). Chimp empirically shows ~0.04444
m/count, attributed to the SX1280 register 0x092B (upper calibration byte) not being written by
RadioLib (only 0x092C is written), leaving an unknown fixed offset.

---

## Firmware: `firmware_calibration/src/main.cpp`

### CAL_TABLE (current state)
```cpp
static const uint16_t CAL_TABLE[3][6] = {
    { 10299, 10271, 10244, 10242, 10230, 10246 },
    { 11486, 11474, 11453, 11426, 11417, 11401 },
    { 13308, 13493, 13528, 13515, 13415, 13376 },  // SF9 [2][4] = 13415 ← iterate this
};
```
Only `[2][4]` is adjusted. Everything else stays fixed.

### `do_ranging()` — key behavior
```cpp
float do_ranging(bool master) {
    isr_fired = false;
    radio.setDio1Action(onDio1);
    radio.startRanging(master, RANGING_ADDR, CAL_TABLE);

    unsigned long t0 = millis();
    while (!isr_fired && millis() - t0 < 300) yield();  // wait up to 300 ms

    float result = radio.getRangingResult();
    if (result == 0.0f) return NAN;  // startup artifact filter
    return result;
}
```

**Important:** DIO1 ISR does NOT reliably fire for ranging events on T3-S3 H594 variant.
The 300 ms ceiling is intentional — the result register IS valid after 300 ms if an exchange
completed, regardless of ISR. Returning NAN for `result == 0.0f` filters startup zeros only.

### Slave loop (no CPU burn — critical fix)
```cpp
void loop() {
#ifndef CAL_MASTER
    do_ranging(false);
    // NO CPU_BURN_MS here — slave must cycle at ~350 ms so it is always in RX
    // when master fires. With matching 400 ms burn times, master and slave
    // phase-lock and master reads stale register values repeatedly.
    float die = temperatureRead();
    ...
```

---

## Known Working Data (OLD slave firmware — INVALID)
These were measured with slave having a 400 ms CPU burn per cycle, causing stale reads.
**DO NOT USE for interpolation — the means are inflated by ~8 m vs. true values.**

| CAL | mean (m) | Note |
|-----|----------|------|
| 12995 | +12.729 | STALE — old slave |
| 13266 | +7.472  | STALE — old slave |
| 13345 | +5.474  | STALE — old slave |
| 13379 | +4.692  | STALE — old slave |
| 13397 | +3.927  | STALE — old slave |
| 13430 | −6.598  | STALE — old slave |

---

## Valid Data (NEW slave firmware — no CPU burn)

| CAL | mean (m) | sigma (m) | err (m) | ok | outlier | timeout |
|-----|----------|-----------|---------|-----|---------|---------|
| 13404 | −4.4004 | 0.6839 | −5.095 | 100 | 0 | 0 |
| 13397 | −4.7184 | 3.0135 | −5.413 | 100 | 0 | 0 |

**Observations:**
1. Both measurements are ~4.4–4.7 m below target (0.695 m). Cal is far too high.
2. The apparent slope between these two points (+0.32 m over 7 counts) is within noise
   (sigma=3 m at cal=13397). Cannot reliably determine direction from these two noisy points.
3. Theoretical slope: higher cal → lower (more negative) reading. We need to go DOWN in cal.

---

## Calibration Script: `tools/auto_cal.py`

Automates: edit `CAL_TABLE[2][4]` in source → pio build+flash → serial monitor → parse RESULT
line → interpolate next cal → repeat until `|mean − 0.695| < 0.03 m`.

### Current bugs (being fixed)

**Bug 1: Stale SEED_DATA poisoned interpolation**
Old-slave data was in SEED_DATA. Script mixed old-slave point `(13397, +3.927)` with
new-slave measurement `(13397, −4.718)`. The interpolation got:
- above target: `(13397, +3.927)` ← old/invalid
- below target: `(13397, −4.718)` ← new/valid
- Both bracket endpoints have the SAME cal=13397 → extrapolation collapses to 13397 → guard
  `next_cal == cal` bumps it by 1 → stuck at 13397–13398 forever.

**Fix:** Clear SEED_DATA except for one clean new-slave data point: `(13404, −4.4004)`.

**Bug 2: All-below extrapolation goes wrong direction**
When all history points are below target (mean < 0.695 m), the two-point extrapolation uses
the apparent positive slope between noisy points (higher cal → higher reading in a 7-count
window within noise), which sends cal UPWARD — opposite of what's needed.

**Fix:** When no bracket exists (all above or all below target), fall back to the empirical
Chimp factor: `next_cal = cal - round(err / 0.04444)`.

With err = −5.095 m at cal=13404:
`delta = round(−5.095 / 0.04444) = −115`
`next_cal = 13404 − (−115) = 13289`

---

## Expected Next Steps

1. Run auto_cal.py with start-cal=13289:
   ```
   python3 tools/auto_cal.py --master-port /dev/ttyACM2 --pio ~/.local/bin/pio
   ```

2. Expected result at cal=13289: reading should be positive (above 0.695 m), establishing a
   proper bracket between 13289 (above) and 13404 (below).

3. Once bracketed, linear interpolation should converge in 2–3 more iterations.

---

## Quick sanity check for the reviewer

If at cal=13404 the reading is −4.4004 m, and the theoretical sensitivity is 0.02253 m/count,
the required cal shift is: 5.095 / 0.02253 ≈ 226 counts down → cal ≈ 13178.

Using Chimp's empirical 0.04444 m/count: 5.095 / 0.04444 ≈ 115 counts down → cal ≈ 13289.

The truth is somewhere in this range. The script with the fallback formula will explore this
region and once it gets a bracket it will interpolate precisely.

---

## Files

- `firmware_calibration/src/main.cpp` — calibration firmware (edit CAL_TABLE[2][4])
- `tools/auto_cal.py` — automation script
- `tools/arch_commands.txt` — flash/monitor commands for Arch laptop
