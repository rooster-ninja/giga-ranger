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
    batch, time_utc, cal, die_c, amb_c, rssi_dbm, mean_m, sigma_m, n_raw, n_rejected

With --save-samples: also writes <logname>_samples.csv with every raw sample tagged
    by batch number. Columns include all per-exchange RF metrics from the LoRa telemetry
    exchange (inst_rssi_dbm, freq_err_hz, lora_rssi_dbm, lora_snr_db, chimp_* fields).

Note: the board must be in RANGING_INFO mode (type "start" in the Alpha serial terminal)
before ranging CSV rows will appear. This script passively reads whatever is on the port.

Requires: pip install pyserial
"""

import re
import sys
import time
import math
import statistics as st
import csv
import argparse
import threading
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

# Matches the mandatory first 6 fields of master CSV rows (split-based parser below)
_ROW_RE = re.compile(
    r'^(\d+),([+-]?\d+\.\d+),([+-]?\d+\.\d+),([+-]?\d+\.\d+|NA),([+-]?\d+\.\d+),(\d+)'
)

def _parse_master_line(line: str) -> dict | None:
    if not _ROW_RE.match(line):
        return None
    p = line.split(',')
    def f(i): return float(p[i]) if len(p) > i and p[i].strip() else float('nan')
    def iv(i): return int(p[i]) if len(p) > i and p[i].strip() else 0
    return {
        't_ms':                p[0],
        'raw_m':               float(p[1]),
        'die_c':               float(p[2]),
        'amb_c':               float('nan') if p[3] == 'NA' else float(p[3]),
        'rssi_dbm':            float(p[4]),
        'gain_step':           int(p[5]),
        'snr_db':              f(6),
        'rssi_sync':           f(7),
        'inst_rssi_dbm':       f(8),
        'freq_err_hz':         f(9),
        'lora_rssi_dbm':       f(10),
        'lora_snr_db':         f(11),
        'chimp_inst_rssi_dbm': f(12),
        'chimp_rssi_sync_dbm': f(13),
        'chimp_snr_db':        f(14),
        'chimp_rssi_corr_dbm': f(15),
        'chimp_gain_step':     iv(16),
        'chimp_freq_err_hz':   f(17),
    }

# Slave CSV: t_ms,die_c,amb_c,rssi_dbm,gain_step,snr_db[,rssi_sync[,inst_rssi_dbm[,freq_err_hz]]]
_SLAVE_RE = re.compile(
    r'^(\d+),([+-]?\d+\.\d+),([+-]?\d+\.\d+|NA),([+-]?\d+\.\d+),(\d+),([+-]?\d+\.\d+)'
    r'(?:,([+-]?\d+\.\d+)(?:,([+-]?\d+\.\d+)(?:,([+-]?\d+\.\d+))?)?)?'
)
# groups: 1=t_ms 2=die_c 3=amb_c 4=rssi 5=gain_step 6=snr_db
#         7=rssi_sync 8=inst_rssi_dbm 9=freq_err_hz


def process_batch(rows: list[tuple], cal: int, batch_n: int) -> tuple[dict, list[bool]]:
    """
    Returns (result_dict, kept_mask) where kept_mask[i] is True if rows[i] survived IQR filter.
    rows: (raw_m, die_c, amb_c, rssi_dbm, gain_step[, snr_db])  gain_step=0 if absent.
    """
    raw_m      = [r[0] for r in rows]
    die_vals   = [r[1] for r in rows]
    amb_vals   = [r[2] for r in rows if not math.isnan(r[2])]
    rssi_vals  = [r[3] for r in rows]
    gain_steps = [r[4] for r in rows]

    q1, _, q3 = st.quantiles(raw_m, n=4)
    iqr = q3 - q1
    lo, hi = q1 - 3.0 * iqr, q3 + 3.0 * iqr
    kept_mask = [lo <= x <= hi for x in raw_m]
    clean_idx  = [i for i, k in enumerate(kept_mask) if k]
    clean_m    = [raw_m[i] for i in clean_idx]
    clean_r    = [rssi_vals[i] for i in clean_idx]
    n_rej = len(raw_m) - len(clean_m)

    if len(clean_m) < 10:
        raise RuntimeError(f"Too few clean samples: {len(clean_m)}/{len(raw_m)}")

    # Gain step mode (most common value); 0 means firmware predates gain logging
    gain_mode = 0
    if any(g > 0 for g in gain_steps):
        from collections import Counter
        gain_mode = Counter(gain_steps).most_common(1)[0][0]

    return {
        "batch":      batch_n,
        "time_utc":   datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "cal":        cal,
        "die_c":      round(st.median(die_vals), 2),
        "amb_c":      round(st.median(amb_vals), 2) if amb_vals else "NA",
        "rssi_dbm":   round(st.mean(clean_r), 1),
        "gain_step":  gain_mode,
        "mean_m":     round(st.mean(clean_m), 4),
        "sigma_m":    round(st.stdev(clean_m), 4),
        "n_raw":      len(raw_m),
        "n_rejected": n_rej,
    }, kept_mask


def _slave_logger(port: str, log_path: Path, stop_event: threading.Event) -> None:
    """Background thread: read Chimp slave CSV rows and write to log_path."""
    fieldnames = ["time_utc", "t_ms", "die_c", "amb_c", "rssi_dbm",
                  "gain_step", "snr_db", "rssi_sync", "inst_rssi_dbm", "freq_err_hz"]
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
                        "time_utc":       datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
                        "t_ms":           m.group(1),
                        "die_c":          m.group(2),
                        "amb_c":          m.group(3),
                        "rssi_dbm":       m.group(4),
                        "gain_step":      m.group(5),
                        "snr_db":         m.group(6),
                        "rssi_sync":      m.group(7) if m.group(7) else "",
                        "inst_rssi_dbm":  m.group(8) if m.group(8) else "",
                        "freq_err_hz":    m.group(9) if m.group(9) else "",
                    })
                    f.flush()
        ser.close()
    except Exception as e:
        print(f"\n[slave] logger error: {e}", file=sys.stderr)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port",  default=DEFAULT_PORT,
                    help="Serial port for master board")
    ap.add_argument("--cal",   type=int, default=DEFAULT_CAL,
                    help="CAL_TABLE[2][4] currently in firmware (for logging only — not modified)")
    ap.add_argument("--batch", type=int, default=DEFAULT_BATCH,
                    help="Samples per measurement batch (default 500, ~360 s)")
    ap.add_argument("--save-samples", action="store_true", default=True,
                    help="(always on) Write every raw sample to <log>_samples.csv")
    ap.add_argument("--slave-port", default=None,
                    help="Also log Chimp (slave) serial output concurrently to <log>_slave.csv")
    args = ap.parse_args()

    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    assets_dir = Path(__file__).resolve().parent.parent / "Assets"
    log_path     = assets_dir / f"thermal_sweep_{ts}.csv"
    samples_path = assets_dir / f"thermal_sweep_{ts}_samples.csv"
    slave_path   = assets_dir / f"thermal_sweep_{ts}_slave.csv"

    fieldnames = ["batch", "time_utc", "cal", "die_c", "amb_c", "rssi_dbm", "gain_step",
                  "mean_m", "sigma_m", "n_raw", "n_rejected"]
    sample_fieldnames = [
        "batch", "t_ms", "raw_m", "die_c", "amb_c", "rssi_dbm",
        "gain_step", "snr_db", "rssi_sync",
        "inst_rssi_dbm", "freq_err_hz",
        "lora_rssi_dbm", "lora_snr_db",
        "chimp_inst_rssi_dbm", "chimp_rssi_sync_dbm", "chimp_snr_db",
        "chimp_rssi_corr_dbm", "chimp_gain_step", "chimp_freq_err_hz",
        "kept",
    ]

    print("=" * 60)
    print("  SX1280 Thermal Regression Sweep")
    print(f"  CAL (fixed) : {args.cal}")
    print(f"  Port        : {args.port}")
    print(f"  Batch size  : {args.batch} samples (~{args.batch * 0.72 / 60:.0f} min each)")
    print(f"  Log file    : {log_path.name}")
    print(f"  Samples log : {samples_path.name}")
    if args.slave_port:
        print(f"  Slave port  : {args.slave_port}  → {slave_path.name}")
    print("=" * 60)
    print(f"\n  Step die temp through plateaus. Wait for die_c to stabilise")
    print(f"  (≤0.5°C variance across 2-3 consecutive batches) before moving on.")
    print(f"  Ctrl-C to stop.\n")
    print(f"  {'batch':>5}  {'die_c':>6}  {'amb_c':>6}  {'rssi':>7}  {'gain':>4}  {'mean_m':>8}  {'sigma_m':>7}  {'rej':>4}")
    print("  " + "─" * 62)

    # Start slave logger thread if requested
    slave_stop = threading.Event()
    slave_thread = None
    if args.slave_port:
        slave_thread = threading.Thread(
            target=_slave_logger,
            args=(args.slave_port, slave_path, slave_stop),
            daemon=True,
        )
        slave_thread.start()
        print(f"[slave] logging {args.slave_port} → {slave_path.name}")

    ser = serial.Serial(args.port, 115200, timeout=3.0)
    time.sleep(2.5)
    ser.reset_input_buffer()

    batch_n = 0
    # rows stores: (raw_m, die_c, amb_c, rssi_dbm, gain_step, t_ms_str, snr_db)
    rows: list[tuple] = []

    with open(log_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        f.flush()

        sample_f = open(samples_path, "w", newline="")
        sample_writer = csv.DictWriter(sample_f, fieldnames=sample_fieldnames)
        sample_writer.writeheader()
        sample_f.flush()

        try:
            while True:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("ascii", errors="replace").strip()
                row = _parse_master_line(line)
                if not row:
                    continue
                rows.append(row)

                if len(rows) >= args.batch:
                    batch_n += 1
                    data_rows = [(r['raw_m'], r['die_c'], r['amb_c'], r['rssi_dbm'], r['gain_step'])
                                 for r in rows]
                    try:
                        result, kept_mask = process_batch(data_rows, args.cal, batch_n)
                    except RuntimeError as e:
                        print(f"  [warn] batch {batch_n} skipped: {e}")
                        rows = []
                        continue

                    writer.writerow(result)
                    f.flush()

                    if sample_writer is not None:
                        for i, r in enumerate(rows):
                            def _ff(v): return f"{v:.1f}" if not math.isnan(v) else ""
                            def _f0(v): return f"{v:.0f}" if not math.isnan(v) else ""
                            sample_writer.writerow({
                                "batch":               batch_n,
                                "t_ms":                r['t_ms'],
                                "raw_m":               f"{r['raw_m']:.4f}",
                                "die_c":               f"{r['die_c']:.1f}",
                                "amb_c":               f"{r['amb_c']:.2f}" if not math.isnan(r['amb_c']) else "NA",
                                "rssi_dbm":            f"{r['rssi_dbm']:.1f}",
                                "gain_step":           r['gain_step'],
                                "snr_db":              _ff(r['snr_db']),
                                "rssi_sync":           _ff(r['rssi_sync']),
                                "inst_rssi_dbm":       _ff(r['inst_rssi_dbm']),
                                "freq_err_hz":         _f0(r['freq_err_hz']),
                                "lora_rssi_dbm":       _ff(r['lora_rssi_dbm']),
                                "lora_snr_db":         _ff(r['lora_snr_db']),
                                "chimp_inst_rssi_dbm": _ff(r['chimp_inst_rssi_dbm']),
                                "chimp_rssi_sync_dbm": _ff(r['chimp_rssi_sync_dbm']),
                                "chimp_snr_db":        _ff(r['chimp_snr_db']),
                                "chimp_rssi_corr_dbm": _ff(r['chimp_rssi_corr_dbm']),
                                "chimp_gain_step":     r['chimp_gain_step'],
                                "chimp_freq_err_hz":   _f0(r['chimp_freq_err_hz']),
                                "kept":                "1" if kept_mask[i] else "0",
                            })
                        sample_f.flush()

                    rows = []
                    amb_disp  = f"{result['amb_c']:>6.1f}" if result['amb_c'] != 'NA' else f"{'NA':>6}"
                    gain_disp = f"{result['gain_step']:>4}" if result['gain_step'] > 0 else f"{'--':>4}"
                    print(f"  {result['batch']:>5}  {result['die_c']:>6.1f}  "
                          f"{amb_disp}  {result['rssi_dbm']:>7.1f}  {gain_disp}  "
                          f"{result['mean_m']:>+8.4f}  {result['sigma_m']:>7.4f}  "
                          f"{result['n_rejected']:>4}")

        except KeyboardInterrupt:
            pass

        sample_f.close()

    slave_stop.set()
    if slave_thread:
        slave_thread.join(timeout=3.0)

    ser.close()
    print(f"\n[sweep] {batch_n} batches written → {log_path}")
    print(f"[sweep] samples written → {samples_path}")
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
