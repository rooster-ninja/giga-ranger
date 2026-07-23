#!/usr/bin/env python3
"""
auto_cal.py — Automated SX1280 ranging calibration (Chimp as master).

Iterates CAL_TABLE[2][4] in firmware_calibration/src/main.cpp until
mean ranging result ≈ 1.4388 m (RG-316 1 m cable, VF 0.695 → apparent = 1 m / 0.695).

Requires: pip install pyserial

Usage (from giga_ranger root or tools/):
    python3 tools/auto_cal.py --master-port /dev/ttyACM2 --pio ~/.local/bin/pio
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

TARGET_M    = 1.0 / 0.695  # RG-316 1 m cable, VF 0.695 → apparent ToF distance = 1.4388 m
TOLERANCE_M = 0.03    # converge when |mean − target| < 30 mm
MAX_ITERS   = 15

SCRIPT_DIR  = Path(__file__).resolve().parent
FW_DIR      = SCRIPT_DIR.parent / "firmware_calibration"
FW_SRC      = FW_DIR / "src" / "main.cpp"

# Seed data — new slave firmware only (no CPU burn on slave).
# Old-slave data is INVALID (stale reads inflated those means by ~8 m); do not use.
SEED_DATA: list[tuple[int, float]] = [
    (13404, -4.4004),   # clean run, sigma=0.68 m, new slave firmware
]

# Chimp empirical sensitivity: 1 cal count ≈ 0.04444 m change in reading.
METERS_PER_COUNT_CHIMP = 0.04444


def set_cal(val: int) -> None:
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
    Open serial port, send SPACE, collect 100-sample run, return (mean_m, sigma_m).
    Port is opened once and kept open for the entire run to avoid ACM renumbering.
    """
    print(f"[serial] Opening {port} at 115200")
    ser = serial.Serial(port, 115200, timeout=3.0)
    time.sleep(2.5)   # wait for board banner
    ser.reset_input_buffer()

    print("[serial] Sending SPACE → starting 100-sample run")
    ser.write(b" ")

    deadline = time.time() + 200   # 100 × ~0.77 s per exchange + margin
    result_line: str | None = None

    while time.time() < deadline:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("ascii", errors="replace").strip()
        if line:
            print(f"  {line}")
        if line.startswith("# RESULT"):
            result_line = line
            break

    ser.close()

    if result_line is None:
        raise RuntimeError("Timed out waiting for RESULT line")

    m = re.search(r"mean=([+-]?[\d.]+)", result_line)
    s = re.search(r"sigma=([\d.]+)", result_line)
    if not m or not s:
        raise RuntimeError(f"Cannot parse RESULT: {result_line!r}")
    return float(m.group(1)), float(s.group(1))


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
    ap.add_argument("--master-port", default="/dev/ttyACM2",
                    help="Serial port for Chimp (master)")
    ap.add_argument("--pio", default="pio",
                    help="Path to pio executable, e.g. ~/.local/bin/pio")
    ap.add_argument("--start-cal", type=int, default=13289,
                    help="Starting CAL_TABLE[2][4] value")
    args = ap.parse_args()

    print("=" * 62)
    print("  SX1280 Auto-Calibration — Chimp master")
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

        if abs(err) <= TOLERANCE_M:
            print(f"\n{'=' * 62}")
            print(f"  CONVERGED   CAL_TABLE[2][4] = {cal}")
            print(f"  mean={mean_m:.4f} m   error={err:+.4f} m   sigma={sigma_m:.4f} m")
            print(f"{'=' * 62}")
            return

        next_cal = interpolate(history, TARGET_M)

        if next_cal is None:
            # No bracket yet — use empirical Chimp factor to step toward target.
            # Higher cal → lower reading (negative slope), so negative error → lower cal.
            delta = round(err / METERS_PER_COUNT_CHIMP)
            next_cal = cal - delta   # err negative → delta negative → cal decreases

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
