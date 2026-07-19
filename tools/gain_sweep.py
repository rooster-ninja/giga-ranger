#!/usr/bin/env python3
"""
gain_sweep.py — Sweep FIXED_GAIN 1-13 and measure ranging mean at each step.

Edits FIXED_GAIN in firmware_calibration/src/main.cpp, reflashes master,
collects one 500-sample batch, records results, then advances to the next step.
Use to characterise group delay vs LNA gain step at a fixed attenuation level.

Known data points (80 dB bench, CAL=13382, 2026-07-19):
  gain=10 → mean=5.40 m, RSSI=-41.3 dBm
  gain=8  → mean=4.36 m, RSSI=-40.4 dBm
  Δ per 2 steps: -1.05 m / -0.9 dBm

Usage (from giga_ranger root):
    python3 tools/gain_sweep.py --port /dev/ttyACM2 --pio ~/.local/bin/pio
    python3 tools/gain_sweep.py --port /dev/ttyACM2 --from-gain 10 --to-gain 1
    python3 tools/gain_sweep.py --port /dev/ttyACM2 --from-gain 13 --to-gain 1 --pio ~/.local/bin/pio

Requires: pip install pyserial
"""

import re
import sys
import time
import os
import argparse
import subprocess
import statistics as st
import csv
from pathlib import Path
from datetime import datetime, timezone

try:
    import serial
except ImportError:
    print("[FATAL] pyserial not installed: pip install pyserial")
    sys.exit(1)

SCRIPT_DIR = Path(__file__).resolve().parent
FW_DIR     = SCRIPT_DIR.parent / "firmware_calibration"
FW_SRC     = FW_DIR / "src" / "main.cpp"
ASSETS_DIR = SCRIPT_DIR.parent / "Assets"

TARGET_M   = 0.695
N_SAMPLES  = 500
TIMEOUT_S  = 450  # 500 samples × ~0.72 s + margin

_CSV_RE = re.compile(
    r'^\d+,([+-]?\d+\.\d+),([+-]?\d+\.\d+),(?:[+-]?\d+\.\d+|NA),([+-]?\d+\.\d+),(\d+)'
)


def set_fixed_gain(gain: int) -> None:
    src = FW_SRC.read_text()
    if not re.search(r'#define\s+FIXED_GAIN\s+\d+', src):
        raise RuntimeError(f"FIXED_GAIN define not found in {FW_SRC} — check firmware source")
    new_src = re.sub(r'(#define\s+FIXED_GAIN\s+)\d+', rf'\g<1>{gain}', src)
    if new_src == src:
        print(f"[fw]  FIXED_GAIN already {gain} — skipping write")
    else:
        FW_SRC.write_text(new_src)
    print(f"[fw]  FIXED_GAIN = {gain}")


def flash(port: str, pio: str) -> None:
    cmd = [os.path.expanduser(pio), "run", "-e", "master", "-t", "upload",
           f"--upload-port={port}"]
    print(f"[flash] {' '.join(cmd)}")
    r = subprocess.run(cmd, cwd=FW_DIR, capture_output=False)
    if r.returncode != 0:
        raise RuntimeError(f"pio flash failed (rc={r.returncode})")


def collect_batch(port: str) -> dict | None:
    """
    Collect N_SAMPLES CSV rows. Returns dict with mean_m, sigma_m, rssi_dbm, gain_step,
    or None if insufficient samples received (signal lost).
    """
    print(f"[serial] Collecting {N_SAMPLES} samples (~{N_SAMPLES * 0.72 / 60:.0f} min)…")
    ser = serial.Serial(port, 115200, timeout=3.0)
    time.sleep(2.5)
    ser.reset_input_buffer()

    raw_m:  list[float] = []
    rssi:   list[float] = []
    gains:  list[int]   = []
    deadline = time.time() + TIMEOUT_S

    while time.time() < deadline and len(raw_m) < N_SAMPLES:
        line = ser.readline().decode("ascii", errors="replace").strip()
        if line:
            print(f"  {line}")
        m = _CSV_RE.match(line)
        if m:
            raw_m.append(float(m.group(1)))
            rssi.append(float(m.group(3)))
            gains.append(int(m.group(4)))

    ser.close()

    if len(raw_m) < 50:
        print(f"  [warn] Only {len(raw_m)} samples — signal likely lost at this gain step")
        return None

    # IQR×3 filter
    n_raw = len(raw_m)
    q1, _, q3 = st.quantiles(raw_m, n=4)
    iqr = q3 - q1
    lo, hi = q1 - 3.0 * iqr, q3 + 3.0 * iqr
    pairs = [(r, s, g) for r, s, g in zip(raw_m, rssi, gains) if lo <= r <= hi]
    n_rej = n_raw - len(pairs)

    if len(pairs) < 10:
        print(f"  [warn] Too few clean samples after filter: {len(pairs)}/{n_raw}")
        return None

    clean_m    = [p[0] for p in pairs]
    clean_rssi = [p[1] for p in pairs]
    clean_gain = [p[2] for p in pairs]

    from collections import Counter
    gain_mode = Counter(clean_gain).most_common(1)[0][0]

    return {
        "mean_m":    st.mean(clean_m),
        "sigma_m":   st.stdev(clean_m),
        "rssi_dbm":  st.mean(clean_rssi),
        "gain_step": gain_mode,
        "n_raw":     n_raw,
        "n_rej":     n_rej,
    }


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port",      default="/dev/ttyACM2")
    ap.add_argument("--pio",       default="pio")
    ap.add_argument("--from-gain", type=int, default=13,
                    help="Start gain step (default 13 = max)")
    ap.add_argument("--to-gain",   type=int, default=1,
                    help="End gain step inclusive (default 1 = min)")
    ap.add_argument("--cal",       type=int, default=13382,
                    help="CAL_TABLE[2][4] in firmware (for logging only)")
    args = ap.parse_args()

    step_dir = -1 if args.from_gain > args.to_gain else 1
    gain_steps = list(range(args.from_gain, args.to_gain + step_dir, step_dir))

    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = ASSETS_DIR / f"gain_sweep_{ts}.csv"
    fieldnames = ["gain_set", "gain_verified", "mean_m", "sigma_m", "rssi_dbm",
                  "n_raw", "n_rejected", "cal", "time_utc"]

    print("=" * 62)
    print("  SX1280 Gain Sweep")
    print(f"  Steps:  {args.from_gain} → {args.to_gain}  ({len(gain_steps)} steps)")
    print(f"  Port:   {args.port}   CAL={args.cal}")
    print(f"  Log:    {log_path.name}")
    print("=" * 62)
    print(f"\n  {'gain':>4}  {'mean_m':>8}  {'sigma_m':>7}  {'rssi':>7}  {'rej':>4}  note")
    print("  " + "─" * 52)

    results = []

    with open(log_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()

        for gain in gain_steps:
            print(f"\n{'─' * 62}")
            print(f"  Gain step {gain}")
            print(f"{'─' * 62}")

            set_fixed_gain(gain)
            flash(args.port, args.pio)
            time.sleep(3.0)

            result = collect_batch(args.port)

            if result is None:
                note = "NO SIGNAL"
                print(f"  {gain:>4}  {'---':>8}  {'---':>7}  {'---':>7}  {'---':>4}  {note}")
                writer.writerow({
                    "gain_set": gain, "gain_verified": 0,
                    "mean_m": "NA", "sigma_m": "NA", "rssi_dbm": "NA",
                    "n_raw": 0, "n_rejected": 0,
                    "cal": args.cal,
                    "time_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                })
                f.flush()
                print(f"\n[gain_sweep] Signal lost at gain={gain}. Stopping.")
                break

            row = {
                "gain_set":      gain,
                "gain_verified": result["gain_step"],
                "mean_m":        round(result["mean_m"], 4),
                "sigma_m":       round(result["sigma_m"], 4),
                "rssi_dbm":      round(result["rssi_dbm"], 1),
                "n_raw":         result["n_raw"],
                "n_rejected":    result["n_rej"],
                "cal":           args.cal,
                "time_utc":      datetime.now(timezone.utc).isoformat(timespec="seconds"),
            }
            writer.writerow(row)
            f.flush()
            results.append(row)

            bias = result["mean_m"] - TARGET_M
            note = f"bias={bias:+.3f} m"
            print(f"  {gain:>4}  {result['mean_m']:>+8.4f}  {result['sigma_m']:>7.4f}  "
                  f"{result['rssi_dbm']:>7.1f}  {result['n_rej']:>4}  {note}")

    # Summary table
    if results:
        print(f"\n{'=' * 62}")
        print(f"  Gain Sweep Summary (CAL={args.cal}, 80 dB bench)")
        print(f"  {'gain':>4}  {'mean_m':>8}  {'sigma_m':>7}  {'rssi':>7}  {'bias_m':>8}")
        print("  " + "─" * 46)
        for r in results:
            bias = float(r["mean_m"]) - TARGET_M
            print(f"  {r['gain_set']:>4}  {float(r['mean_m']):>+8.4f}  "
                  f"{float(r['sigma_m']):>7.4f}  {float(r['rssi_dbm']):>7.1f}  {bias:>+8.4f}")
        print(f"{'=' * 62}")
        print(f"\n[gain_sweep] {len(results)} steps written → {log_path}")


if __name__ == "__main__":
    main()
