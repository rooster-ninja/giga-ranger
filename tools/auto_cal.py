#!/usr/bin/env python3
"""
auto_cal.py — Automated SX1280 ranging calibration (Alpha as master).

Iterates CAL_TABLE[2][4] in firmware_calibration/src/main.cpp until
mean ranging result ≈ 0.695 m (RG-316 1 m cable, VF 0.695).

Firmware must be free-run master (no SPACE trigger). Waits for the
first # [n=100] stats line after each flash (~70 s per iteration).

Requires: pip install pyserial

Usage (from giga_ranger root or tools/):
    python3 tools/auto_cal.py --master-port /dev/ttyACM1 --pio ~/.local/bin/pio
"""

import re
import sys
import time
import os
import argparse
import subprocess
from pathlib import Path

try:
    import serial
except ImportError:
    print("[FATAL] pyserial not installed: pip install pyserial")
    sys.exit(1)

TARGET_M    = 0.695   # RG-316 1m × VF 0.695
TOLERANCE_M = 0.03    # converge when |mean − target| < 30 mm
MAX_ITERS   = 15

SCRIPT_DIR  = Path(__file__).resolve().parent
FW_DIR      = SCRIPT_DIR.parent / "firmware_calibration"
FW_SRC      = FW_DIR / "src" / "main.cpp"

# Seed data — new slave firmware only (no CPU burn on slave).
# Old-slave data is INVALID (stale reads inflated those means by ~8 m); do not use.
SEED_DATA: list[tuple[int, float]] = []  # populated per-board via --start-cal

# Theoretical sensitivity: 150 / (4.096 × 1625) = 0.02253 m/count.
# BW=1625 confirmed in both firmware and RadioLib's setBandwidth().
# Previous empirical 0.04444 was an artifact of stale slave data inflating means.
METERS_PER_COUNT_CHIMP = 0.02253


CAL_MIN = 10000
CAL_MAX = 16000


def set_cal(val: int) -> None:
    if not (CAL_MIN <= val <= CAL_MAX):
        raise ValueError(f"cal {val} out of range [{CAL_MIN}, {CAL_MAX}] — bad measurement?")
    src = FW_SRC.read_text()
    # Match SF9 row by its trailing element 13376 — flexible about all other values/spacing
    new_src = re.sub(
        r'(\{\s*\d+,\s*\d+,\s*\d+,\s*\d+,\s*)\d+(\s*,\s*13376\s*\})',
        rf'\g<1>{val}\g<2>',
        src,
    )
    # Keep the inline comment accurate
    new_src = re.sub(
        r'(\[2\]\[4\]\s*=\s*)\d+',
        rf'\g<1>{val}',
        new_src,
    )
    # Verify pattern exists (substitution may be a no-op if val already matches current value)
    if not re.search(r'\{\s*\d+,\s*\d+,\s*\d+,\s*\d+,\s*\d+\s*,\s*13376\s*\}', src):
        for line in src.splitlines():
            if "13376" in line or "SF9" in line:
                print(f"  [debug] {repr(line)}")
        raise RuntimeError(
            f"CAL_TABLE pattern not found in {FW_SRC}\n"
            "Expected a 6-element array row ending with 13376."
        )
    FW_SRC.write_text(new_src)
    print(f"[fw]  CAL_TABLE[2][4] = {val}")


def flash(port: str, pio: str) -> None:
    cmd = [os.path.expanduser(pio), "run", "-e", "master", "-t", "upload",
           f"--upload-port={port}"]
    print(f"[flash] {' '.join(cmd)}")
    r = subprocess.run(cmd, cwd=FW_DIR)
    if r.returncode != 0:
        raise RuntimeError(f"pio flash failed (rc={r.returncode})")


def run_sample(port: str) -> tuple[float, float]:
    """
    Collect 500 individual CSV samples from master firmware, apply IQR×3 outlier
    rejection, return (filtered_mean_m, filtered_sigma_m).

    Firmware emits individual lines as CSV "t_ms,raw_m,die_c,amb_c" while ranging.
    At n=500 it emits "# [n=500] mean=... sigma=..." as a "done" trigger.
    Samples within ±100 m of zero pass the firmware gate but may still be large
    negative outliers (-10 to -30 m) caused by stale-register reads after timeouts.
    IQR×3 filtering removes them without touching firmware.

    500 samples × ~0.72 s each ≈ 360 s; deadline is 420 s.
    """
    import statistics as _stats

    _CSV_RE = re.compile(r'^\d+,([+-]?\d+\.\d+),')

    print(f"[serial] Opening {port} at 115200")
    ser = serial.Serial(port, 115200, timeout=3.0)
    time.sleep(2.5)
    ser.reset_input_buffer()

    print("[serial] Waiting for first 500-sample stats line (~360 s)...")
    deadline = time.time() + 420
    done = False
    raw_samples: list[float] = []

    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("ascii", errors="replace").strip()
        if line:
            print(f"  {line}")
        m = _CSV_RE.match(line)
        if m:
            raw_samples.append(float(m.group(1)))
        if "# [n=" in line and "mean=" in line:
            done = True
            break

    ser.close()

    if not done:
        raise RuntimeError("Timed out waiting for # [n=] stats line")
    if len(raw_samples) < 10:
        raise RuntimeError(f"Too few individual samples received: {len(raw_samples)}")

    # IQR×3 outlier rejection
    n_raw = len(raw_samples)
    q1, _, q3 = _stats.quantiles(raw_samples, n=4)
    iqr = q3 - q1
    lo, hi = q1 - 3.0 * iqr, q3 + 3.0 * iqr
    clean = [x for x in raw_samples if lo <= x <= hi]
    n_rejected = n_raw - len(clean)

    if len(clean) < 10:
        raise RuntimeError(f"Too few clean samples after IQR filter: {len(clean)}/{n_raw}")

    mean_m  = _stats.mean(clean)
    sigma_m = _stats.stdev(clean)
    print(f"  [filter] n_raw={n_raw}  rejected={n_rejected}  n_clean={len(clean)}")
    print(f"  [filter] mean={mean_m:.4f} m   sigma={sigma_m:.4f} m")

    return mean_m, sigma_m


def interpolate(data: list[tuple[int, float]], target: float) -> int | None:
    """
    Linear interpolation between the tightest bracket straddling the target.
    Returns None when no valid bracket exists (all points on same side).
    """
    # Deduplicate by cal: keep the most recent measurement for each cal value.
    by_cal: dict[int, float] = {}
    for c, m in data:
        by_cal[c] = m   # later entries overwrite earlier ones
    pts = sorted(by_cal.items())   # ascending by cal

    above = [(c, m) for c, m in pts if m >= target]
    below = [(c, m) for c, m in pts if m < target]

    if not (above and below):
        return None   # no bracket yet

    c0, m0 = max(above, key=lambda x: x[0])  # highest cal still reading > target
    c1, m1 = min(below, key=lambda x: x[0])  # lowest cal already reading < target

    if m0 == m1:
        return (c0 + c1) // 2

    cal_f = c0 + (target - m0) * (c1 - c0) / (m1 - m0)
    return round(cal_f)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--master-port", default="/dev/ttyACM1",
                    help="Serial port for Alpha (master)")
    ap.add_argument("--pio", default="pio",
                    help="Path to pio executable, e.g. ~/.local/bin/pio")
    ap.add_argument("--start-cal", type=int, default=13316,
                    help="Starting CAL_TABLE[2][4] value (13316 = Alpha convergence at die~36°C/amb~25.5°C, 2026-07-17)")
    args = ap.parse_args()

    print("=" * 62)
    print("  SX1280 Auto-Calibration — Alpha master")
    print(f"  Target:  {TARGET_M} m  ±{TOLERANCE_M} m")
    print(f"  Port:    {args.master_port}")
    print(f"  PIO:     {args.pio}")
    print("=" * 62)

    history: list[tuple[int, float]] = list(SEED_DATA)
    cal = args.start_cal

    for it in range(1, MAX_ITERS + 1):
        print(f"\n{'─' * 62}")
        print(f"  Iteration {it}/{MAX_ITERS}   CAL = {cal}")
        print(f"{'─' * 62}")

        set_cal(cal)
        flash(args.master_port, args.pio)
        time.sleep(3.0)   # board reboot after flash

        mean_m, sigma_m = run_sample(args.master_port)
        err = mean_m - TARGET_M

        print(f"\n>>> cal={cal}  mean={mean_m:.4f} m  sigma={sigma_m:.4f} m  "
              f"error={err:+.4f} m")

        history.append((cal, mean_m))

        if mean_m > 50.0:
            print(f"\n[ABORT] mean={mean_m:.1f} m — cable not connected or boards too far apart.")
            sys.exit(1)

        if abs(err) <= TOLERANCE_M:
            print(f"\n{'=' * 62}")
            print(f"  CONVERGED   CAL_TABLE[2][4] = {cal}")
            print(f"  mean={mean_m:.4f} m   error={err:+.4f} m   sigma={sigma_m:.4f} m")
            print(f"{'=' * 62}")
            return

        next_cal = interpolate(history, TARGET_M)

        if next_cal is None:
            # Higher cal → lower reading. Step toward target.
            # err > 0 (reading too high): delta > 0, cal increases → reading drops.
            # err < 0 (reading too low):  delta < 0, cal decreases → reading rises.
            delta = round(err / METERS_PER_COUNT_CHIMP)
            next_cal = cal + delta

        # Guard: never suggest the same value we just measured
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
