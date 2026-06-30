# LoRa-Ranger — Project Brief & Web Deployment Handoff

A 2.4 GHz time-of-flight **distance-measuring** link between two fixed sites, built on the Semtech SX1280. This document hands the project to Claude Code to build the **web interface** (aiming + live readout) that runs on the radio nodes.

---

## 1. What the system does

Two SX1280 radios measure the distance between them via round-trip time-of-flight ranging. One node is the **initiator** (sends ranging requests, computes distance), the other is the **responder** (auto-replies). Both sites are **fixed**; one sits on a mountaintop for clear line of sight. Target link distance is **~60 km**, with comfortable margin.

---

## 2. Hardware (per site — build two identical)

| Component | Choice | Notes |
|---|---|---|
| MCU + radio | **LILYGO T3-S3, SX1280 "without PA" version** | ESP32-S3 + SX1280 integrated. "Without PA" is **required** — internal RF switching is what makes ranging work. The PA (M27S-style) version breaks ranging. |
| Antenna | **2.4 GHz directional Yagi, 13 dBi, SMA-female** | Ordered in the SMA-**female** variant so it mates **directly** to the T3-S3 board jack — no adapter, no pigtail, no extra loss. VNA-verified: SWR ≈ 1.11 @ 2.45 GHz, well-matched across 2.40–2.49 GHz. Realized gain ~11 dBi. Beamwidth E=48° / H=75°. |
| Weather sensor | **BME280 (I²C)** | Feeds the atmospheric delay-correction model. Must be BME280 (has humidity), not BMP280. |
| Enclosure / mast | IP65 **plastic** box (RF-transparent), steel mast + U-bolts | Weatherproof the SMA joint; add surge arrestor + ground if mast-mounting high. |
| Power | USB mains **or** small solar + LiPo (T3-S3 has onboard charger) | Non-PA radio draws little, so solar is easy. |

---

## 3. Radio configuration

- **Band:** 2.4 GHz ISM (SX1280 is 2.4 GHz only).
- **Frequency:** 2450 MHz.
- **Bandwidth:** 1625 kHz (widest — best distance resolution).
- **Spreading factor:** SF9–10 (margin is plentiful; no need to max it).
- **Output power:** chip set to 13 dBm (max).
- **Roles:** one initiator, one responder; identical RF params on both or they won't pair.

**Link budget (sanity):** 12 dBm TX + ~11 dBi ×2 antennas − 136 dB path loss (60 km) − ~4 dB misc ≈ −106 dBm RX, vs ~−124 dBm ranging sensitivity → **~18 dB margin**. Power is not the constraint; the radio horizon (antenna height + earth curvature) is.

---

## 4. Software status

- **Firmware library options:** RadioLib (C++, mature ranging API: `range()`, `getRangingResult()`) **or** Rust on ESP32-S3 (`esp-hal`/`esp-idf`). Lowest-risk Rust path = FFI-wrap Semtech's C SX1280 driver for the ranging engine, app logic in Rust.
- **Calibration:** Both sites are at known surveyed coordinates → known true distance. Measure what the SX1280 reports, subtract the constant offset once. Average several readings per measurement to cut jitter.
- **Atmospheric (GRIN) delay correction:** See `grin_delay.rs` (separate file). Computes the troposphere's excess path (~15 m / ~50 ns over 60 km) from BME280 readings. Apply only the **change** vs calibration-day conditions on top of the surveyed-distance offset.

---

## 5. WEB DEPLOYMENT — the task for Claude Code

Build a web interface **served by the ESP32-S3 node itself** (it has WiFi). Two purposes:

### 5a. Aiming mode (install-time)
A live page showing **RSSI and SNR as large, glanceable numbers**, updating ~2–4× per second, so a person at the mast can peak the antenna while watching their **phone** instead of a laptop.
- Big numeric RSSI/SNR readout + a simple bar or trend sparkline to see the peak.
- Audible/visual "getting stronger / weaker" cue is a bonus.
- Goal: sweep antenna slowly, find main-lobe peak, lock the mount.

### 5b. Operations dashboard (run-time)
A live page showing the ranging output:
- **Corrected distance** (m), updating per measurement cycle.
- Raw vs corrected distance, current applied offset, and current atmospheric correction.
- Live **BME280** readings (P, T, RH) feeding the GRIN model.
- Link health: RSSI, SNR, valid/total ranging success rate.
- Optional: simple rolling history chart of distance + RSSI.

### Technical constraints
- **Host on-device** (ESP32-S3): serve over its WiFi (AP mode for field install, or STA mode if a network is present). Keep assets small — single self-contained HTML page, inline CSS/JS, no heavy frameworks; data via a lightweight JSON endpoint or WebSocket polled from the page.
- **No external dependencies at runtime** (field use = no internet). All assets embedded in firmware flash.
- Must run comfortably within ESP32-S3 RAM/flash; avoid large libraries.
- Mobile-first layout (used on a phone at the mast). High-contrast, large type, readable in sunlight.
- Endpoints to expose (suggested): `/` (page), `/api/status` (JSON: rssi, snr, raw_m, corrected_m, offset_m, atmo_corr_m, p, t, rh, success_rate), optional `/ws` for push updates.

### Suggested next steps for Claude Code
1. Stand up the on-device web server (AP mode) with a stub `/api/status` returning mock data.
2. Build the single-page mobile UI (aiming view + dashboard view, toggle between them).
3. Wire real RSSI/SNR + ranging results + BME280 into `/api/status`.
4. Add WebSocket push for the ~3 Hz aiming refresh.
5. Embed assets into firmware for internet-free field operation.

---

## 6. Install notes (context for any UI cues)

- **Polarization:** Yagi polarization follows element orientation — **both ends must match** (both vertical or both horizontal); 90° mismatch ≈ −20 dB. Decide once, note on both install sheets.
- **Aiming:** wide beam (48°×75°) is forgiving (±24° within 3 dB). Pre-aim by GPS great-circle bearing (correct for ~14–15° E magnetic declination in eastern BC), then fine-peak on RSSI.
- **Diagnostics:** if RSSI is low despite good aim, rotate one antenna 90° (polarization check); also verify Fresnel-zone + earth-curvature clearance at the midpoint before blaming aim.

---

## 7. Power: 12 VDC battery → 5 VDC (clean, quiet)

The T3-S3 is powered by **5 V** (via its USB/5 V input) and regulates to 3.3 V on-board. At a solar/battery site the source is a **12 V battery**, so a DC-DC stage is needed. Priorities: **high efficiency** (battery/solar budget) and **low noise** (supply ripple can add timing jitter to ranging).

### Recommended approach

| Tier | Solution | Why |
|---|---|---|
| **Simple + clean (recommended)** | **RECOM R-78E5.0-1.0** switching module (12 V → 5 V, 1 A) | Drop-in sealed DC-DC, high efficiency, low noise, wide input. One part, no tuning. Comfortable for the node's ~0.5 A draw (1 A headroom). |
| **Cheapest** | Quality buck module (e.g. Pololu **D24V10F5**, 5 V/1 A) | Pre-filtered, cleaner than bare MP1584 boards. Add output caps. |
| **Quietest** | **Buck → LDO two-stage**: buck to ~5.6 V, then low-dropout LDO to 5 V | Buck does the efficient heavy lift; LDO scrubs switching ripple. Use when you want the cleanest possible rail. |

Whichever tier: add **bulk + ceramic decoupling** at both converter input and output (e.g. 100 µF electrolytic ∥ 0.1 µF ceramic each side), and keep the converter and its loops **away from the antenna/RF section**.

### Input protection (essential on a battery/solar source)

A 12 V battery — especially behind a solar charge controller — can sag to ~10.5 V, rise to ~14.6 V while charging, and throw transient spikes. Protect the front end:

- **Reverse-polarity protection** — series Schottky or, better, a P-FET ideal-diode (lower loss).
- **Inline fuse** sized just above peak current (e.g. 1 A) close to the battery.
- **TVS diode** (e.g. SMBJ16A-class) across the 12 V input to clamp surges.
- Confirm the converter's input range spans **~10–16 V** (R-78 and most bucks do).
- Common ground between battery, converter, and board.

### LiPo ride-through (optional UPS)

The T3-S3 has an onboard LiPo charger. Feeding it regulated 5 V both runs the board **and** trickle-charges a small LiPo, which then carries the node through cloudy spells / brief battery dips — a free uninterruptible buffer for an off-grid mountaintop site.

### Quiet-power notes for ranging

- 2.4 GHz is far from buck switching frequencies, so direct in-band interference is unlikely; the real risk is **supply ripple → clock/PLL jitter → noisier distance readings**. Clean 5 V + the board's 3.3 V LDO together keep this low.
- A small **ferrite bead** on the 5 V feed and short, twisted DC leads further reduce conducted noise.
- If you observe ranging jitter that tracks load, suspect supply noise first — scope the 5 V rail for ripple before chasing RF.

---

## 8. Reference files
- `grin_delay.rs` — atmospheric excess-delay model (BME280 → distance correction).
- `sx1280_ranging_master.ino` / `sx1280_ranging_slave.ino` — RadioLib reference sketches (C++) for initiator/responder, if used as a starting point or cross-reference.
