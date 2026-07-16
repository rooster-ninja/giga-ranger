# SX1280 Calibration Debug Summary

## ⚠️ Board Rename (2026-07-16)
Roles permanently swapped. Old "Chimp" is now **Alpha** (master). Old "Alpha" is now **Chimp** (slave).
ACM port assignments stay the same — only the names changed.

| New name | Old name | Role       | ACM port | CAL_TABLE[2][4] |
|----------|----------|------------|----------|-----------------|
| Alpha    | Chimp    | Master     | ACM2     | **13229**       |
| Chimp    | Alpha    | Slave      | ACM1     | 13115 (as master, for reference only) |

**Production cal (Alpha as master): 13229**

## Final Calibration Results (clean data, new slave fw, 2026-07-16)

| Master | CAL_TABLE[2][4] | mean (m) | sigma (m) | note |
|--------|-----------------|----------|-----------|------|
| Alpha (was Chimp) | **13229** | 0.7187 | 0.5961 | clean — use this |
| Chimp (was Alpha) | 13115     | 0.7169 | 3.2309 | noisy — for reference |

Using 13229 (new Alpha's value). Roles will not swap in production.

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
