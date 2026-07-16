#!/usr/bin/env python3
"""
auto_cal.py — Automated SX1280 ranging calibration (Chimp as master).

Iterates CAL_TABLE[2][4] in firmware_calibration/src/main.cpp until
mean ranging result ≈ 0.695 m (RG-316 1 m cable, VF 0.695).

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

TARGET_M    = 0.695   # RG-316 1m × VF 0.695
TOLERANCE_M = 0.03    # converge when |mean − target| < 30 mm
MAX_ITERS   = 15

SCRIPT_DIR  = Path(__file__).resolve().parent
FW_DIR      = SCRIPT_DIR.parent / "firmware_calibration"
FW_SRC      = FW_DIR / "src" / "main.cpp"

# Prior collected data points (cal, mean_m) from earlier sessions.
# Seeds the bracketing interpolation from the first iteration.
SEED_DATA: list[tuple[int, float]] = [
    (12995, 12.729),
    (13266,  7.472),
    (13345,  5.474),
    (13379,  4.692),
    (13397,  3.927),
    (13430, -6.598),
]


def set_cal(val: int) -> None:
    src = FW_SRC.read_text()
    # Replace the 5th element of the SF9 row: { 13308, 13493, 13528, 13515, XXXXX, 13376 }
    new_src = re.sub(
        r'(\{ 13308, 13493, 13528, 13515, )\d+(, 13376 \})',
        rf'\g<1>{val}\g<2>',
        src,
    )
    # Keep the inline comment accurate
    new_src = re.sub(
        r'(// SF9 \[2\]\[4\] = )\d+',
        rf'\g<1>{val}',
        new_src,
    )
    if new_src == src:
        raise RuntimeError("CAL_TABLE pattern not found in main.cpp — check regex")
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


def interpolate(data: list[tuple[int, float]], target: float) -> int:
    """
    Linear interpolation/extrapolation between the two data points that
    most tightly bracket the target mean.  Falls back to the two nearest
    points when no bracket exists.
    """
    pts = sorted(set(data))   # unique, ascending by cal

    above = [(c, m) for c, m in pts if m >= target]  # mean too high → cal too low
    below = [(c, m) for c, m in pts if m < target]   # mean too low  → cal too high

    if above and below:
        c0, m0 = max(above, key=lambda x: x[0])  # highest cal still reading > target
        c1, m1 = min(below, key=lambda x: x[0])  # lowest  cal already reading < target
    elif above:
        (c0, m0), (c1, m1) = pts[-1], pts[-2]
    else:
        (c0, m0), (c1, m1) = pts[0], pts[1]

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
    ap.add_argument("--start-cal", type=int, default=13415,
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
