#!/usr/bin/env python3
"""
gain_sweep.py — Multi-pass gain sweep for SX1280 ranging bias characterisation.

Iterates through gain steps 13→1, then 1→13, then 13→1 (configurable number of
passes, alternating direction). Flashes master at each step, collects N_SAMPLES
ranging exchanges, logs per-sample and per-step summary CSVs. Optionally logs
Chimp (slave) serial output concurrently throughout the entire run.

Usage (from giga_ranger root):
    python3 tools/gain_sweep.py --port /dev/ttyACM2 --pio ~/.local/bin/pio
    python3 tools/gain_sweep.py --port /dev/ttyACM2 --slave-port /dev/ttyACM1
    python3 tools/gain_sweep.py --port /dev/ttyACM2 --from-gain 13 --to-gain 1 --passes 3

Requires: pip install pyserial
"""

import re
import sys
import time
import os
import math
import argparse
import subprocess
import statistics as st
import csv
import threading
from pathlib import Path
from datetime import datetime, timezone
from collections import Counter

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
    r'^(\d+),([+-]?\d+\.\d+),([+-]?\d+\.\d+),([+-]?\d+\.\d+|NA),([+-]?\d+\.\d+),(\d+)'
    r'(?:,([+-]?\d+\.\d+)(?:,([+-]?\d+\.\d+))?)?'
)
# groups: 1=t_ms  2=raw_m  3=die_c  4=amb_c  5=rssi  6=gain_step  7=snr_db  8=rssi_sync

_SLAVE_RE = re.compile(
    r'^(\d+),([+-]?\d+\.\d+),([+-]?\d+\.\d+|NA),([+-]?\d+\.\d+),(\d+),([+-]?\d+\.\d+)'
    r'(?:,([+-]?\d+\.\d+))?'
)
# groups: 1=t_ms  2=die_c  3=amb_c  4=rssi  5=gain_step  6=snr_db  7=rssi_sync


def set_fixed_gain(gain: int) -> None:
    src = FW_SRC.read_text()
    if not re.search(r'#define\s+FIXED_GAIN\s+\d+', src):
        raise RuntimeError(f"FIXED_GAIN define not found in {FW_SRC}")
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


def collect_batch(port: str) -> tuple[dict, list[dict]] | tuple[None, None]:
    """
    Collect N_SAMPLES CSV rows from master serial port.
    Returns (summary_dict, raw_samples) or (None, None) if insufficient samples.
    """
    print(f"[serial] Collecting {N_SAMPLES} samples (~{N_SAMPLES * 0.72 / 60:.0f} min)…")
    ser = serial.Serial(port, 115200, timeout=3.0)
    time.sleep(2.5)
    ser.reset_input_buffer()

    # (raw_m, rssi, gain, t_ms, die_c, amb_c, snr_db, rssi_sync)
    samples: list[tuple] = []
    deadline = time.time() + TIMEOUT_S

    while time.time() < deadline and len(samples) < N_SAMPLES:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("ascii", errors="replace").strip()
        # Only print non-data lines (stats, errors) to keep output manageable
        if line and not _CSV_RE.match(line):
            print(f"  {line}")
        m = _CSV_RE.match(line)
        if m:
            amb_str   = m.group(4)
            snr_str   = m.group(7)
            rsync_str = m.group(8)
            samples.append((
                float(m.group(2)),                                           # raw_m
                float(m.group(5)),                                           # rssi
                int(m.group(6)),                                             # gain
                m.group(1),                                                  # t_ms
                float(m.group(3)),                                           # die_c
                float(amb_str) if amb_str != "NA" else float("nan"),         # amb_c
                float(snr_str) if snr_str is not None else float("nan"),     # snr_db
                float(rsync_str) if rsync_str is not None else float("nan"), # rssi_sync
            ))
            # Print progress every 100 samples
            n = len(samples)
            if n % 100 == 0:
                print(f"  [{n}/{N_SAMPLES}] rssi={samples[-1][1]:.1f} gain={samples[-1][2]}")

    ser.close()

    if len(samples) < 50:
        print(f"  [warn] Only {len(samples)} samples — signal likely lost at this gain step")
        return None, None

    raw_m = [s[0] for s in samples]
    n_raw = len(raw_m)

    q1, _, q3 = st.quantiles(raw_m, n=4)
    iqr = q3 - q1
    lo, hi = q1 - 3.0 * iqr, q3 + 3.0 * iqr
    kept_mask = [lo <= r <= hi for r in raw_m]
    clean = [s for s, k in zip(samples, kept_mask) if k]
    n_rej = n_raw - len(clean)

    if len(clean) < 10:
        print(f"  [warn] Too few clean samples after filter: {len(clean)}/{n_raw}")
        return None, None

    clean_m    = [s[0] for s in clean]
    clean_rssi = [s[1] for s in clean]
    clean_gain = [s[2] for s in clean]
    gain_mode  = Counter(clean_gain).most_common(1)[0][0]

    summary = {
        "mean_m":    st.mean(clean_m),
        "sigma_m":   st.stdev(clean_m),
        "rssi_dbm":  st.mean(clean_rssi),
        "gain_step": gain_mode,
        "n_raw":     n_raw,
        "n_rej":     n_rej,
    }

    raw_rows = [
        {
            "t_ms":      s[3],
            "raw_m":     f"{s[0]:.4f}",
            "die_c":     f"{s[4]:.1f}",
            "amb_c":     f"{s[5]:.2f}" if not math.isnan(s[5]) else "NA",
            "rssi_dbm":  f"{s[1]:.1f}",
            "gain_step": s[2],
            "snr_db":    f"{s[6]:.1f}" if not math.isnan(s[6]) else "",
            "rssi_sync": f"{s[7]:.1f}" if not math.isnan(s[7]) else "",
            "kept":      "1" if kept_mask[i] else "0",
        }
        for i, s in enumerate(samples)
    ]

    return summary, raw_rows


def _slave_logger(port: str, log_path: Path, stop_event: threading.Event) -> None:
    """Background thread: read Chimp slave CSV rows and write to log_path."""
    fieldnames = ["time_utc", "t_ms", "die_c", "amb_c", "rssi_dbm",
                  "gain_step", "snr_db", "rssi_sync"]
    try:
        ser = serial.Serial(port, 115200, timeout=3.0)
        time.sleep(2.5)
        ser.reset_input_buffer()
        with open(log_path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            f.flush()
            while not stop_event.is_set():
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("ascii", errors="replace").strip()
                m = _SLAVE_RE.match(line)
                if m:
                    writer.writerow({
                        "time_utc":  datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
                        "t_ms":      m.group(1),
                        "die_c":     m.group(2),
                        "amb_c":     m.group(3),
                        "rssi_dbm":  m.group(4),
                        "gain_step": m.group(5),
                        "snr_db":    m.group(6),
                        "rssi_sync": m.group(7) if m.group(7) is not None else "",
                    })
                    f.flush()
        ser.close()
    except Exception as e:
        print(f"\n[slave] logger error: {e}", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port",       default="/dev/ttyACM2",
                    help="Serial port for master board")
    ap.add_argument("--slave-port", default=None,
                    help="Serial port for Chimp (slave) board — logged concurrently")
    ap.add_argument("--pio",        default="pio",
                    help="Path to pio binary (default: pio)")
    ap.add_argument("--from-gain",  type=int, default=13,
                    help="Starting gain step for pass 1 (default 13)")
    ap.add_argument("--to-gain",    type=int, default=1,
                    help="Ending gain step for pass 1 (default 1)")
    ap.add_argument("--passes",     type=int, default=3,
                    help="Number of passes, alternating direction (default 3)")
    ap.add_argument("--cal",        type=int, default=13382,
                    help="CAL_TABLE[2][4] in firmware (for logging only)")
    ap.add_argument("--save-samples", action="store_true", default=True,
                    help="Write every raw sample to <log>_samples.csv")
    args = ap.parse_args()

    # Build pass list: pass 1 uses from→to, pass 2 reverses, pass 3 repeats from→to, etc.
    lo = min(args.from_gain, args.to_gain)
    hi = max(args.from_gain, args.to_gain)
    start_high = args.from_gain >= args.to_gain  # True → pass 1 goes 13→1

    all_gain_sequences: list[list[int]] = []
    for p in range(args.passes):
        from_high = start_high if p % 2 == 0 else not start_high
        if from_high:
            seq = list(range(hi, lo - 1, -1))   # e.g. [13,12,...,1]
        else:
            seq = list(range(lo, hi + 1))         # e.g. [1,2,...,13]
        all_gain_sequences.append(seq)

    total_steps = sum(len(s) for s in all_gain_sequences)
    est_hours   = total_steps * N_SAMPLES * 0.72 / 3600

    ts           = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_path     = ASSETS_DIR / f"gain_sweep_{ts}.csv"
    samples_path = ASSETS_DIR / f"gain_sweep_{ts}_samples.csv"
    slave_path   = ASSETS_DIR / f"gain_sweep_{ts}_slave.csv"

    fieldnames = ["pass_num", "gain_set", "gain_verified", "mean_m", "sigma_m",
                  "rssi_dbm", "n_raw", "n_rejected", "cal", "time_utc"]
    sample_fieldnames = ["pass_num", "gain_set", "t_ms", "raw_m", "die_c", "amb_c",
                         "rssi_dbm", "gain_step", "snr_db", "rssi_sync", "kept"]

    print("=" * 68)
    print("  SX1280 Multi-Pass Gain Sweep")
    print(f"  Passes:  {args.passes}  (alternating {args.from_gain}→{args.to_gain} / {args.to_gain}→{args.from_gain})")
    print(f"  Steps:   {total_steps} total  (~{est_hours:.1f} h estimated)")
    print(f"  Samples: {N_SAMPLES} per step")
    print(f"  Port:    {args.port}   CAL={args.cal}")
    print(f"  Log:     {log_path.name}")
    if args.save_samples:
        print(f"  Samples: {samples_path.name}")
    if args.slave_port:
        print(f"  Slave:   {args.slave_port}  → {slave_path.name}")
    print("=" * 68)

    # Start slave logger thread
    slave_stop   = threading.Event()
    slave_thread = None
    if args.slave_port:
        slave_thread = threading.Thread(
            target=_slave_logger,
            args=(args.slave_port, slave_path, slave_stop),
            daemon=True,
        )
        slave_thread.start()
        print(f"[slave] logging {args.slave_port} → {slave_path.name}")

    all_results: list[dict] = []

    sample_f      = None
    sample_writer = None
    if args.save_samples:
        sample_f      = open(samples_path, "w", newline="")
        sample_writer = csv.DictWriter(sample_f, fieldnames=sample_fieldnames)
        sample_writer.writeheader()
        sample_f.flush()

    try:
        with open(log_path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()

            for pass_num, gain_seq in enumerate(all_gain_sequences, start=1):
                direction = "↓" if gain_seq[0] > gain_seq[-1] else "↑"
                print(f"\n{'═' * 68}")
                print(f"  PASS {pass_num}/{args.passes}  {direction}  steps {gain_seq[0]}→{gain_seq[-1]}")
                print(f"  {'gain':>4}  {'mean_m':>8}  {'sigma_m':>7}  {'rssi':>7}  {'gain_v':>6}  {'rej':>4}  note")
                print(f"  {'─' * 56}")

                for gain in gain_seq:
                    print(f"\n  ── gain={gain}  pass={pass_num} ──")
                    set_fixed_gain(gain)
                    flash(args.port, args.pio)
                    time.sleep(3.0)

                    result, raw_rows = collect_batch(args.port)

                    if result is None:
                        note = "NO SIGNAL"
                        print(f"  {gain:>4}  {'---':>8}  {'---':>7}  {'---':>7}  {'---':>6}  {'---':>4}  {note}")
                        writer.writerow({
                            "pass_num": pass_num, "gain_set": gain, "gain_verified": 0,
                            "mean_m": "NA", "sigma_m": "NA", "rssi_dbm": "NA",
                            "n_raw": 0, "n_rejected": 0, "cal": args.cal,
                            "time_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                        })
                        f.flush()
                        print(f"  [warn] Signal lost at gain={gain} pass={pass_num}. Continuing to next step.")
                        continue

                    if sample_writer is not None and raw_rows:
                        for r in raw_rows:
                            sample_writer.writerow({"pass_num": pass_num, "gain_set": gain, **r})
                        sample_f.flush()

                    row = {
                        "pass_num":      pass_num,
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
                    all_results.append(row)

                    bias = result["mean_m"] - TARGET_M
                    note = f"bias={bias:+.3f} m"
                    print(f"  {gain:>4}  {result['mean_m']:>+8.4f}  {result['sigma_m']:>7.4f}  "
                          f"{result['rssi_dbm']:>7.1f}  {result['gain_step']:>6}  "
                          f"{result['n_rej']:>4}  {note}")

    except KeyboardInterrupt:
        print("\n[gain_sweep] Interrupted by user.")

    if sample_f is not None:
        sample_f.close()

    slave_stop.set()
    if slave_thread:
        slave_thread.join(timeout=3.0)

    # Restore FIXED_GAIN=0 (AGC) after sweep
    set_fixed_gain(0)

    # Final summary table grouped by gain, across all passes
    if all_results:
        print(f"\n{'═' * 68}")
        print(f"  Final Summary  CAL={args.cal}  passes={args.passes}")
        print(f"  {'pass':>4}  {'gain':>4}  {'mean_m':>8}  {'sigma_m':>7}  {'rssi':>7}  {'bias_m':>8}")
        print(f"  {'─' * 52}")
        for r in all_results:
            try:
                bias = float(r["mean_m"]) - TARGET_M
                print(f"  {r['pass_num']:>4}  {r['gain_set']:>4}  {float(r['mean_m']):>+8.4f}  "
                      f"{float(r['sigma_m']):>7.4f}  {float(r['rssi_dbm']):>7.1f}  {bias:>+8.4f}")
            except (ValueError, TypeError):
                print(f"  {r['pass_num']:>4}  {r['gain_set']:>4}  {'NO SIGNAL':>8}")
        print(f"{'═' * 68}")
        print(f"\n[gain_sweep] {len(all_results)} steps written → {log_path}")
        if args.save_samples:
            print(f"[gain_sweep] samples written → {samples_path}")
        if args.slave_port:
            print(f"[gain_sweep] slave log      → {slave_path}")


if __name__ == "__main__":
    main()
