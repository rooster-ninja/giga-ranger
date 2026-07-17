#!/usr/bin/env python3
"""
thermal_sweep.py — Fixed-CAL temperature regression sweep for SX1280 ranging.

Does NOT flash firmware or modify CAL_TABLE. Assumes firmware_calibration is already
running on the master board with the desired CAL. Reads continuous CSV output, batches
BATCH_SIZE samples, applies IQR×3 filtering, and appends one row per batch to a
timestamped CSV log.

Workflow:
    1. Flash firmware_calibration with fixed CAL (e.g. CAL=13316):
           python3 tools/auto_cal.py --master-port /dev/ttyACM1 --pio ~/.local/bin/pio
           (or manually: edit main.cpp, pio run -e master -t upload ...)
    2. Run this script:
           python3 tools/thermal_sweep.py --port /dev/ttyACM1 --cal 13316
    3. Step through temperature points. Die temp appears in each batch line.
       Wait for die_c to stabilise (≤0.5°C variation across 2-3 consecutive batches)
       before considering that plateau settled.
    4. Ctrl-C when done. CSV is flushed after every batch.

Output CSV columns:
    batch, time_utc, cal, die_c, amb_c, mean_m, sigma_m, n_raw, n_rejected

Requires: pip install pyserial
"""

import re
import sys
import time
import statistics as st
import csv
import argparse
from pathlib import Path
from datetime import datetime, timezone

try:
    import serial
except ImportError:
    print("[FATAL] pyserial not installed: pip install pyserial")
    sys.exit(1)

DEFAULT_BATCH   = 500
DEFAULT_PORT    = "/dev/ttyACM1"
DEFAULT_CAL     = 13316

# Matches individual master CSV rows: t_ms,raw_m,die_c,amb_c
_ROW_RE = re.compile(r'^\d+,([+-]?\d+\.\d+),([+-]?\d+\.\d+),([+-]?\d+\.\d+)')


def process_batch(rows: list[tuple[float, float, float]], cal: int, batch_n: int) -> dict:
    raw_m = [r[0] for r in rows]
    die_vals = [r[1] for r in rows]
    amb_vals = [r[2] for r in rows]

    q1, _, q3 = st.quantiles(raw_m, n=4)
    iqr = q3 - q1
    lo, hi = q1 - 3.0 * iqr, q3 + 3.0 * iqr
    clean = [x for x in raw_m if lo <= x <= hi]
    n_rej = len(raw_m) - len(clean)

    if len(clean) < 10:
        raise RuntimeError(f"Too few clean samples: {len(clean)}/{len(raw_m)}")

    return {
        "batch":      batch_n,
        "time_utc":   datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "cal":        cal,
        "die_c":      round(st.median(die_vals), 2),
        "amb_c":      round(st.median(amb_vals), 2),
        "mean_m":     round(st.mean(clean), 4),
        "sigma_m":    round(st.stdev(clean), 4),
        "n_raw":      len(raw_m),
        "n_rejected": n_rej,
    }


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port",  default=DEFAULT_PORT,
                    help="Serial port for master board")
    ap.add_argument("--cal",   type=int, default=DEFAULT_CAL,
                    help="CAL_TABLE[2][4] currently in firmware (for logging only — not modified)")
    ap.add_argument("--batch", type=int, default=DEFAULT_BATCH,
                    help="Samples per measurement batch (default 500, ~360 s)")
    args = ap.parse_args()

    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path = Path(__file__).resolve().parent.parent / "Assets" / f"thermal_sweep_{ts}.csv"
    fieldnames = ["batch", "time_utc", "cal", "die_c", "amb_c",
                  "mean_m", "sigma_m", "n_raw", "n_rejected"]

    print("=" * 60)
    print("  SX1280 Thermal Regression Sweep")
    print(f"  CAL (fixed) : {args.cal}")
    print(f"  Port        : {args.port}")
    print(f"  Batch size  : {args.batch} samples (~{args.batch * 0.72 / 60:.0f} min each)")
    print(f"  Log file    : {log_path.name}")
    print("=" * 60)
    print(f"\n  Step die temp through plateaus. Wait for die_c to stabilise")
    print(f"  (≤0.5°C variance across 2-3 consecutive batches) before moving on.")
    print(f"  Ctrl-C to stop.\n")
    print(f"  {'batch':>5}  {'die_c':>6}  {'amb_c':>6}  {'mean_m':>8}  {'sigma_m':>7}  {'rej':>4}")
    print("  " + "─" * 48)

    ser = serial.Serial(args.port, 115200, timeout=3.0)
    time.sleep(2.5)
    ser.reset_input_buffer()

    batch_n = 0
    rows: list[tuple[float, float, float]] = []

    with open(log_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        f.flush()

        try:
            while True:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("ascii", errors="replace").strip()
                m = _ROW_RE.match(line)
                if not m:
                    continue
                rows.append((float(m.group(1)), float(m.group(2)), float(m.group(3))))

                if len(rows) >= args.batch:
                    batch_n += 1
                    try:
                        result = process_batch(rows, args.cal, batch_n)
                    except RuntimeError as e:
                        print(f"  [warn] batch {batch_n} skipped: {e}")
                        rows = []
                        continue
                    writer.writerow(result)
                    f.flush()
                    rows = []
                    print(f"  {result['batch']:>5}  {result['die_c']:>6.1f}  "
                          f"{result['amb_c']:>6.1f}  {result['mean_m']:>+8.4f}  "
                          f"{result['sigma_m']:>7.4f}  {result['n_rejected']:>4}")

        except KeyboardInterrupt:
            pass

    ser.close()
    print(f"\n[sweep] {batch_n} batches written → {log_path}")
    if batch_n >= 3:
        print("\n  Paste the CSV into the regression notebook or run:")
        print(f"  python3 -c \"\nimport csv,statistics as s\n"
              f"rows=list(csv.DictReader(open('{log_path}')))\n"
              f"xs=[float(r['die_c']) for r in rows]\n"
              f"ys=[float(r['mean_m']) for r in rows]\n"
              f"n=len(xs); xm=s.mean(xs); ym=s.mean(ys)\n"
              f"slope=sum((x-xm)*(y-ym) for x,y in zip(xs,ys))/sum((x-xm)**2 for x in xs)\n"
              f"intercept=ym-slope*xm\n"
              f"print(f'slope={{slope:.4f}} m/degC   intercept={{intercept:.4f}} m')\n\"")


if __name__ == "__main__":
    main()
