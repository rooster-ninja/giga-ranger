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
