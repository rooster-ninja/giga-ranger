# Giga Ranger — TODO

## Calibration Firmware (`firmware_calibration/`)

- [ ] **Outlier filter — statistical (2-pass):** Replace static gate (`m < -8.0 || m > 2.0`) with a proper statistical filter. Proposed approach: first pass collects all samples and computes median + MAD (median absolute deviation); second pass rejects samples beyond 3× MAD from median. This adapts automatically as the calibration value converges toward the target — no manual gate adjustment needed between iterations.

## Production Ranging Firmware (`firmware_ranging/`)

- [ ] **Outlier filter — delta gate + rolling median:** For production, a static distance gate can't be used since true distance is unknown. Recommended approach: (1) **delta gate** — reject any reading that differs from the previous valid reading by more than a threshold; (2) **rolling median** of last N readings (e.g. N=5) as the published value. Median is naturally resistant to single-sample spikes. Delta gate catches glitches immediately; rolling median smooths residual noise.
- [ ] **Discuss delta gate threshold:** Initial suggestion ±500 m — may be reducible. The link is fixed LOS at ~60 km; legitimate distance change between ranging intervals is effectively zero (mm/day from thermal/atmospheric effects). Even ±50 m or ±10 m may be appropriate depending on ranging interval and expected environmental variation. **Test empirically at deployment: run extended session, observe natural reading spread, set threshold at ~10× the observed σ.** Discuss and set final value before production deployment.

## SF9 Calibration (complete)
- [x] Complete iterative SF9 calibration runs — CAL_TABLE[2][4] = 13089, residual −27 mm (2026-07-04)
- [x] Update `firmware_calibration/README.md` — SF9 as primary, SF10 to appendix, Alpha/Chimp-001 naming
- [x] Update calibration notebook on debserv — SF9 as primary section
- [x] Deploy updated notebook to rooster.ninja
- [ ] Update memory (`project_sx1280_ranging.md`) with final SF9 values

## Production Firmware (`firmware_ranging/`)
- [x] Create `firmware_ranging/` PlatformIO project
- [x] Two build environments: `[env:alpha]` (master) and `[env:chimp-001]` (slave)
- [x] Continuous ranging loop with configurable interval (RANGING_INTERVAL_MS, default 5s)
- [x] BLE NUS monitoring — GR-ALPHA / GR-CHIMP001, one line per exchange
- [x] OLED display rotated 90° (portrait): role, BLE status, range, RSSI, SNR, OK count, die temp
- [x] Outlier filter: delta gate (±500 m) + rolling median N=5
- [x] BME280 I²C on GPIO17/GPIO18 (shared bus with OLED)
- [x] No WiFi, no MQTT, no provisioning
- [ ] **Field test:** verify range reads ~60 km at deployment, observe natural σ, tighten DELTA_GATE_M
- [ ] **BME280 data in BLE output:** add T/H/P fields to Alpha's BLE NUS line for atmospheric logging

## Website / Docs
- [ ] Update project overview page (rooster.ninja) — SF9 as production SF, Alpha/Chimp-001 naming
- [ ] All docs: replace "Master/Slave", "Board 1/Board 2", "Run A/Run B" → Alpha/Chimp-001
- [ ] Rename calibration sections: primary = "Chimp Calibration"
- [ ] AN1200.29 role-reversal method → appendix footnote only

## Discussion
- [ ] Arduino timing concern vs Rust: SX1280 hardware owns all precision-critical timing; firmware jitter only affects inter-exchange gap (coarse, seconds). Rust would not improve ranging precision. Needs brief write-up in docs.
