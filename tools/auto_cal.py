#!/usr/bin/env python3
"""
auto_cal.py — Automated SX1280 ranging calibration (Alpha as master).

Protocol:
  1. Flash Chimp (slave) once at startup via --slave-port.
  2. Each iteration: update CAL_TABLE[2][4] in firmware, flash Alpha (master).
  3. Open Alpha serial; wait for "# LINK ESTABLISHED".
  4. Send "start\\n" → Alpha enters RANGING_INFO mode (18-column CSV).
  5. Collect --samples CSV rows; extract raw_m (column 1).
  6. Send "stop\\n" → Alpha returns to LORA_LINK.
  7. Apply IQR×3 filter, compute mean/sigma; iterate until converged.

Correct target = 1.0 / 0.695 = 1.4388 m (RG-316 1 m cable, VF 0.695).

Requires: pip install pyserial

Usage:
    python3 tools/auto_cal.py \\
        --master-port /dev/ttyACM2 \\
        --slave-port  /dev/ttyACM0 \\
        --pio ~/.local/bin/pio
"""

import re
import sys
import time
import os
import math
import argparse
import subprocess
import statistics as _stats
from pathlib import Path

try:
    import serial
except ImportError:
    print("[FATAL] pyserial not installed: pip install pyserial")
    sys.exit(1)

TARGET_M    = 1.0 / 0.695  # RG-316 1 m cable, VF 0.695 → apparent ToF distance = 1.4388 m
TOLERANCE_M = 0.03          # converge when |mean − target| < 30 mm
MAX_ITERS   = 15

SCRIPT_DIR = Path(__file__).resolve().parent
FW_DIR     = SCRIPT_DIR.parent / "firmware_calibration"
FW_SRC     = FW_DIR / "src" / "main.cpp"

# Theoretical sensitivity: 150 / (4.096 × 1625) = 0.02253 m/count (SF9/BW1625)
METERS_PER_COUNT = 0.02253

CAL_MIN = 10000
CAL_MAX = 16000


def set_fixed_gain(gain: int) -> None:
    src = FW_SRC.read_text()
    if not re.search(r'#define\s+FIXED_GAIN\s+\d+', src):
        raise RuntimeError(f"FIXED_GAIN define not found in {FW_SRC}")
    new_src = re.sub(r'(#define\s+FIXED_GAIN\s+)\d+', rf'\g<1>{gain}', src)
    if new_src == src:
        print(f"[fw]  FIXED_GAIN already {gain} — skipping write")
        return
    FW_SRC.write_text(new_src)
    print(f"[fw]  FIXED_GAIN = {gain}")


def set_cal(val: int) -> None:
    if not (CAL_MIN <= val <= CAL_MAX):
        raise ValueError(f"cal {val} out of range [{CAL_MIN}, {CAL_MAX}]")
    src = FW_SRC.read_text()
    # Replace CAL_TABLE[2][4] — 5th element of SF9 row; anchor on 6th = 13376
    new_src = re.sub(
        r'(\{\s*\d+,\s*\d+,\s*\d+,\s*\d+,\s*)\d+(\s*,\s*13376\s*\})',
        rf'\g<1>{val}\g<2>',
        src,
    )
    if not re.search(r'\{\s*\d+,\s*\d+,\s*\d+,\s*\d+,\s*\d+\s*,\s*13376\s*\}', src):
        raise RuntimeError(
            f"CAL_TABLE SF9 row pattern not found in {FW_SRC}\n"
            "Expected a 6-element array row ending with 13376."
        )
    FW_SRC.write_text(new_src)
    print(f"[fw]  CAL_TABLE[2][4] = {val}")


def flash(port: str, pio: str, env: str) -> None:
    cmd = [os.path.expanduser(pio), "run", "-e", env, "-t", "upload",
           f"--upload-port={port}"]
    print(f"[flash] {' '.join(cmd)}")
    r = subprocess.run(cmd, cwd=FW_DIR)
    if r.returncode != 0:
        raise RuntimeError(f"pio flash failed (rc={r.returncode})")


def run_sample(port: str, n_samples: int) -> tuple[float, float, float]:
    """
    Establish RANGING_INFO session on Alpha, collect n_samples CSV rows,
    return (filtered_mean_m, filtered_sigma_m, mean_rssi_dbm).

    CSV columns: t_ms(0), raw_m(1), die_c(2), amb_c(3), rssi_dbm(4), ...
    """
    print(f"[serial] Opening {port} at 115200")
    ser = serial.Serial(port, 115200, timeout=1.0)
    ser.reset_input_buffer()

    # Wait for LINK ESTABLISHED (~5 s board boot + <1 s Chimp re-link)
    print("[serial] Waiting for LINK ESTABLISHED (up to 90 s)...")
    deadline = time.time() + 90.0
    linked = False
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("ascii", errors="replace").strip()
        print(f"  {line}")
        if "LINK ESTABLISHED" in line:
            linked = True
            break

    if not linked:
        ser.close()
        raise RuntimeError("Timeout waiting for LINK ESTABLISHED — is Chimp powered and flashed?")

    # Drain heartbeat lines, then trigger ranging
    time.sleep(1.5)
    ser.reset_input_buffer()
    ser.write(b"start\n")
    print("[serial] Sent 'start'")

    # Confirm RANGING_INFO entry (Alpha echoes "# MODE: RANGING_INFO" + CSV header)
    deadline = time.time() + 15.0
    ranging_active = False
    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("ascii", errors="replace").strip()
        print(f"  {line}")
        if "MODE: RANGING_INFO" in line:
            ranging_active = True
        if ranging_active and line.startswith("t_ms,"):
            break

    if not ranging_active:
        ser.close()
        raise RuntimeError("Timeout waiting for RANGING_INFO — did start command get through?")

    # Collect CSV data rows: (raw_m, rssi_dbm)
    samples: list[tuple[float, float]] = []
    deadline = time.time() + n_samples * 3.0 + 30.0
    print(f"[serial] Collecting {n_samples} samples...")

    while len(samples) < n_samples and time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("ascii", errors="replace").strip()
        print(f"  {line}")
        if not line or line.startswith("#"):
            continue
        parts = line.split(",")
        if len(parts) < 5:
            continue
        try:
            int(parts[0])  # must be numeric t_ms — skips CSV header row
            raw_m = float(parts[1])
            rssi  = float(parts[4]) if parts[4] else float("nan")
            samples.append((raw_m, rssi))
        except (ValueError, IndexError):
            continue
        if len(samples) % 50 == 0 and len(samples) > 0:
            print(f"  [progress] {len(samples)}/{n_samples}")

    # Stop ranging
    ser.write(b"stop\n")
    print("[serial] Sent 'stop'")
    time.sleep(1.0)
    ser.close()

    if len(samples) < 10:
        raise RuntimeError(f"Too few samples collected: {len(samples)}")

    # IQR×3 outlier rejection on raw_m
    n_raw  = len(samples)
    raw_ms = [s[0] for s in samples]
    q1, _, q3 = _stats.quantiles(raw_ms, n=4)
    iqr = q3 - q1
    lo, hi = q1 - 3.0 * iqr, q3 + 3.0 * iqr
    clean = [(r, s) for r, s in samples if lo <= r <= hi]
    n_rejected = n_raw - len(clean)

    if len(clean) < 10:
        raise RuntimeError(f"Too few clean samples after IQR filter: {len(clean)}/{n_raw}")

    clean_m    = [p[0] for p in clean]
    clean_rssi = [p[1] for p in clean if not math.isnan(p[1])]

    mean_m    = _stats.mean(clean_m)
    sigma_m   = _stats.stdev(clean_m) if len(clean_m) > 1 else 0.0
    mean_rssi = _stats.mean(clean_rssi) if clean_rssi else float("nan")

    print(f"  [filter] n_raw={n_raw}  rejected={n_rejected}  n_clean={len(clean_m)}")
    print(f"  [filter] mean={mean_m:.4f} m   sigma={sigma_m:.4f} m   rssi={mean_rssi:.1f} dBm")

    return mean_m, sigma_m, mean_rssi


def interpolate(data: list[tuple[int, float]], target: float) -> int | None:
    # Deduplicate — keep most recent for each cal value
    by_cal: dict[int, float] = {}
    for c, m in data:
        by_cal[c] = m
    pts = sorted(by_cal.items())

    above = [(c, m) for c, m in pts if m >= target]
    below = [(c, m) for c, m in pts if m < target]
    if not (above and below):
        return None

    c0, m0 = max(above, key=lambda x: x[0])
    c1, m1 = min(below, key=lambda x: x[0])
    if m0 == m1:
        return (c0 + c1) // 2

    return round(c0 + (target - m0) * (c1 - c0) / (m1 - m0))


def main() -> None:
    ap = argparse.ArgumentParser(description="SX1280 auto-calibration — Alpha master / Chimp slave")
    ap.add_argument("--master-port", required=True,
                    help="Serial port for Alpha (master), e.g. /dev/ttyACM2")
    ap.add_argument("--slave-port", required=True,
                    help="Serial port for Chimp (slave), e.g. /dev/ttyACM0")
    ap.add_argument("--pio", required=True,
                    help="Path to pio executable, e.g. ~/.local/bin/pio")
    ap.add_argument("--start-cal", type=int, default=13316,
                    help="Starting CAL_TABLE[2][4] (default: 13316)")
    ap.add_argument("--samples", type=int, default=200,
                    help="Samples per iteration (default: 200 ≈ 200 s at ~1 s/exchange)")
    ap.add_argument("--slave-gain", type=int, default=10,
                    help="FIXED_GAIN to flash Chimp with (default 10); master always uses same value")
    args = ap.parse_args()

    print("=" * 62)
    print("  SX1280 Auto-Calibration — Alpha master / Chimp slave")
    print(f"  Target:     {TARGET_M:.4f} m  ±{TOLERANCE_M} m")
    print(f"  Master:     {args.master_port}")
    print(f"  Slave:      {args.slave_port}")
    print(f"  PIO:        {args.pio}")
    print(f"  Samples:    {args.samples} per iteration")
    print(f"  Slave gain: FIXED_GAIN={args.slave_gain}")
    print("=" * 62)

    # Flash Chimp (slave) once — firmware does not change between iterations.
    # Explicitly set FIXED_GAIN so calibration is consistent with gain_sweep runs.
    print(f"\n[init] Flashing Chimp (slave) with FIXED_GAIN={args.slave_gain}…")
    set_fixed_gain(args.slave_gain)
    flash(args.slave_port, args.pio, "slave")
    print("[init] Chimp flashed — waiting 5 s for boot + SEEK...")
    time.sleep(5.0)

    history: list[tuple[int, float]] = []
    cal = args.start_cal

    for it in range(1, MAX_ITERS + 1):
        print(f"\n{'─' * 62}")
        print(f"  Iteration {it}/{MAX_ITERS}   CAL = {cal}")
        print(f"{'─' * 62}")

        set_cal(cal)
        flash(args.master_port, args.pio, "master")
        # run_sample() opens serial and waits for LINK ESTABLISHED — no sleep needed here

        mean_m, sigma_m, mean_rssi = run_sample(args.master_port, args.samples)
        err = mean_m - TARGET_M

        print(f"\n>>> cal={cal}  mean={mean_m:.4f} m  sigma={sigma_m:.4f} m  "
              f"rssi={mean_rssi:.1f} dBm  error={err:+.4f} m")

        history.append((cal, mean_m))

        if mean_m > 50.0:
            print(f"\n[ABORT] mean={mean_m:.1f} m — check cable/connection.")
            sys.exit(1)

        if abs(err) <= TOLERANCE_M:
            print(f"\n{'=' * 62}")
            print(f"  CONVERGED   CAL_TABLE[2][4] = {cal}")
            print(f"  mean={mean_m:.4f} m   error={err:+.4f} m   sigma={sigma_m:.4f} m")
            print(f"{'=' * 62}")
            return

        next_cal = interpolate(history, TARGET_M)
        if next_cal is None:
            # Higher CAL → lower reading. Positive err means reading too high → increase CAL.
            delta = round(err / METERS_PER_COUNT)
            next_cal = cal + delta

        if next_cal == cal:
            next_cal = cal + (1 if err < 0 else -1)

        print(f"    → next estimate: {next_cal}")
        cal = next_cal

    print(f"\n[FAIL] Did not converge in {MAX_ITERS} iterations")
    print("Collected data:")
    for c, m in sorted(history):
        print(f"  cal={c:5d}   mean={m:+8.4f} m")
    sys.exit(1)


if __name__ == "__main__":
    main()
