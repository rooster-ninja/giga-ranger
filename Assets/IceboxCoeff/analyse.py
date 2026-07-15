"""
Giga Ranger — Operation Icebox temperature coefficient analysis
Master: master_20260715_160119.csv  (t_ms, raw_m, die_c, amb_c)
Slave:  slave_20260715_160124.csv   (t_ms, die_c, amb_c)
"""

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from scipy import stats
from sklearn.linear_model import LinearRegression

# ── Monokai Pro palette ───────────────────────────────────────────────────────
BG      = '#2D2A2E'
PANEL   = '#221F22'
FG      = '#FCFCFA'
GREY    = '#727072'
YELLOW  = '#FFD866'
ORANGE  = '#FC9867'
RED     = '#FF6188'
GREEN   = '#A9DC76'
BLUE    = '#78DCE8'
PURPLE  = '#AB9DF2'

ASSETS = '/Users/dek/claude_projects/giga_ranger/Assets'
OUT    = f'{ASSETS}/IceboxCoeff'

# ── Load data ─────────────────────────────────────────────────────────────────
def load_master(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            parts = line.split(',')
            if len(parts) != 4:
                continue
            try:
                t, r, d, a = float(parts[0]), float(parts[1]), float(parts[2]), float(parts[3])
                rows.append((t, r, d, a))
            except ValueError:
                continue
    df = pd.DataFrame(rows, columns=['t_ms', 'raw_m', 'alpha_die', 'alpha_amb'])
    return df

def load_slave(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            parts = line.split(',')
            if len(parts) != 3:
                continue
            try:
                t, d, a = float(parts[0]), float(parts[1]), float(parts[2])
                rows.append((t, d, a))
            except ValueError:
                continue
    df = pd.DataFrame(rows, columns=['t_ms', 'chimp_die', 'chimp_amb'])
    return df

master = load_master(f'{ASSETS}/master_20260715_160119.csv')
slave  = load_slave(f'{ASSETS}/slave_20260715_160124.csv')

print(f"Master raw rows: {len(master)}")
print(f"Slave  raw rows: {len(slave)}")

# ── Filter master ─────────────────────────────────────────────────────────────
master = master[(master['raw_m'] != 0.0) & (master['raw_m'] > -20.0) & (master['raw_m'] < 30.0)].copy()
print(f"Master after filter: {len(master)}")

# ── Align slave to master by nearest t_ms ────────────────────────────────────
slave_sorted = slave.sort_values('t_ms').reset_index(drop=True)
master_sorted = master.sort_values('t_ms').reset_index(drop=True)

chimp_die = np.interp(master_sorted['t_ms'], slave_sorted['t_ms'], slave_sorted['chimp_die'])
chimp_amb = np.interp(master_sorted['t_ms'], slave_sorted['t_ms'], slave_sorted['chimp_amb'])
master_sorted['chimp_die'] = chimp_die
master_sorted['chimp_amb'] = chimp_amb

df = master_sorted.copy()
df['t_min'] = df['t_ms'] / 60000.0
df['avg_amb'] = (df['alpha_amb'] + df['chimp_amb']) / 2.0
df['avg_die'] = (df['alpha_die'] + df['chimp_die']) / 2.0

# ── Phase labels ──────────────────────────────────────────────────────────────
# Cold: amb < 8°C (stabilised in icebox)
# Transition/ramp: 8–32°C
# Hot: amb > 32°C (under warming lamps)
# Descent: after peak (t > t_peak)
t_peak = df.loc[df['avg_amb'].idxmax(), 't_ms']
df['phase'] = 'ramp'
df.loc[df['avg_amb'] < 8.0, 'phase'] = 'cold'
df.loc[(df['avg_amb'] > 32.0), 'phase'] = 'hot'
df.loc[(df['t_ms'] > t_peak) & (df['avg_amb'] <= 32.0) & (df['avg_amb'] > 8.0), 'phase'] = 'descent'

phase_colors = {'cold': BLUE, 'ramp': YELLOW, 'hot': RED, 'descent': GREEN}

print(f"\nPhase counts:")
print(df['phase'].value_counts())
print(f"\nAmb range: {df['avg_amb'].min():.2f} – {df['avg_amb'].max():.2f} °C")
print(f"Die range: {df['avg_die'].min():.2f} – {df['avg_die'].max():.2f} °C")
print(f"raw_m range: {df['raw_m'].min():.3f} – {df['raw_m'].max():.3f} m")

# ── Linear regression: raw_m vs avg_amb ──────────────────────────────────────
slope_amb, intercept_amb, r_amb, p_amb, se_amb = stats.linregress(df['avg_amb'], df['raw_m'])
print(f"\nSingle-var (avg_amb):  slope={slope_amb:.4f} m/°C  R²={r_amb**2:.4f}  p={p_amb:.2e}")

# ── Multivariate regression: raw_m vs alpha_amb + chimp_amb ──────────────────
X = df[['alpha_amb', 'chimp_amb']].values
y = df['raw_m'].values
model = LinearRegression().fit(X, y)
r2_multi = model.score(X, y)
coef_alpha = model.coef_[0]
coef_chimp = model.coef_[1]
print(f"Multi-var:  alpha_coef={coef_alpha:.4f}  chimp_coef={coef_chimp:.4f}  R²={r2_multi:.4f}")

# ── Per-phase regression ──────────────────────────────────────────────────────
print("\nPer-phase (avg_amb):")
for phase in ['cold', 'ramp', 'hot', 'descent']:
    sub = df[df['phase'] == phase]
    if len(sub) < 10:
        continue
    sl, ic, r, p, _ = stats.linregress(sub['avg_amb'], sub['raw_m'])
    print(f"  {phase:10s}: n={len(sub):4d}  slope={sl:.4f} m/°C  R²={r**2:.4f}")

# ── Fig 1 — Time series ───────────────────────────────────────────────────────
fig, axes = plt.subplots(3, 1, figsize=(14, 10), facecolor=BG)
fig.suptitle('Operation Icebox — Temperature Characterisation', color=FG, fontsize=14, fontweight='bold')

ax1, ax2, ax3 = axes
for ax in axes:
    ax.set_facecolor(PANEL)
    ax.tick_params(colors=GREY)
    for spine in ax.spines.values():
        spine.set_edgecolor(GREY)
    ax.grid(True, color=GREY, alpha=0.2, linewidth=0.5)

ax1.scatter(df['t_min'], df['raw_m'], s=1, c=YELLOW, alpha=0.3, rasterized=True)
# rolling median to show thermal trend clearly
roll = df.set_index('t_min')['raw_m'].rolling(window=60, center=True).median()
ax1.plot(roll.index, roll.values, color=ORANGE, lw=2, label='60-sample median')
ax1.set_ylabel('raw_m (m)', color=FG)
ax1.set_title('Ranging result (scatter + rolling median)', color=FG, fontsize=10)
ax1.legend(facecolor=PANEL, edgecolor=GREY, labelcolor=FG, fontsize=8)

ax2.plot(df['t_min'], df['alpha_die'], color=ORANGE, lw=1, label='Alpha die')
ax2.plot(df['t_min'], df['chimp_die'],  color=RED,    lw=1, label='Chimp die')
ax2.set_ylabel('Die temp (°C)', color=FG)
ax2.legend(facecolor=PANEL, edgecolor=GREY, labelcolor=FG, fontsize=8)
ax2.set_title('ESP32 die temperature', color=FG, fontsize=10)

ax3.plot(df['t_min'], df['alpha_amb'], color=BLUE,   lw=1, label='Alpha BME')
ax3.plot(df['t_min'], df['chimp_amb'], color=PURPLE, lw=1, label='Chimp BME')
ax3.set_ylabel('Ambient (°C)', color=FG)
ax3.set_xlabel('Time (min)', color=FG)
ax3.legend(facecolor=PANEL, edgecolor=GREY, labelcolor=FG, fontsize=8)
ax3.set_title('BME280 ambient temperature', color=FG, fontsize=10)

# Phase boundary annotations on ax3
t_transition = df[df['phase'] == 'cold']['t_min'].max()
t_lamps_off  = df.loc[df['avg_amb'].idxmax(), 't_min']
for ax in axes:
    ax.axvline(t_transition, color=BLUE,   lw=1, ls='--', alpha=0.6)
    ax.axvline(t_lamps_off,  color=RED,    lw=1, ls='--', alpha=0.6)
ax3.text(t_transition + 0.5, ax3.get_ylim()[0] + 1, 'lamps on', color=BLUE,   fontsize=7)
ax3.text(t_lamps_off  + 0.5, ax3.get_ylim()[0] + 1, 'lamps off', color=RED,   fontsize=7)

plt.tight_layout()
plt.savefig(f'{OUT}/fig1_timeseries.png', dpi=150, bbox_inches='tight', facecolor=BG)
plt.close()
print("Saved fig1_timeseries.png")

# ── Fig 2 — Scatter: raw_m vs avg_amb, coloured by phase ─────────────────────
fig, ax = plt.subplots(figsize=(10, 7), facecolor=BG)
ax.set_facecolor(PANEL)
ax.tick_params(colors=GREY)
for spine in ax.spines.values():
    spine.set_edgecolor(GREY)
ax.grid(True, color=GREY, alpha=0.2, linewidth=0.5)

for phase, color in phase_colors.items():
    sub = df[df['phase'] == phase]
    ax.scatter(sub['avg_amb'], sub['raw_m'], s=2, c=color, alpha=0.5,
               label=phase, rasterized=True)

# Fit line
x_fit = np.linspace(df['avg_amb'].min(), df['avg_amb'].max(), 200)
y_fit = slope_amb * x_fit + intercept_amb
ax.plot(x_fit, y_fit, color=FG, lw=2, ls='--',
        label=f'Linear fit: {slope_amb:+.4f} m/°C  R²={r_amb**2:.4f}')

ax.set_xlabel('Avg BME ambient (°C)', color=FG)
ax.set_ylabel('raw_m (m)', color=FG)
ax.set_title('Ranging result vs ambient temperature — full run', color=FG, fontsize=13)
ax.legend(facecolor=PANEL, edgecolor=GREY, labelcolor=FG, fontsize=9,
          markerscale=4)

plt.tight_layout()
plt.savefig(f'{OUT}/fig2_regression.png', dpi=150, bbox_inches='tight', facecolor=BG)
plt.close()
print("Saved fig2_regression.png")

# ── Fig 3 — Residuals / per-bin mean ─────────────────────────────────────────
bins = np.arange(0, df['avg_amb'].max() + 4, 4)
df['bin'] = pd.cut(df['avg_amb'], bins=bins)
bin_stats = df.groupby('bin', observed=True)['raw_m'].agg(['mean', 'std', 'count']).reset_index()
bin_stats = bin_stats[bin_stats['count'] >= 20]
bin_mid = bin_stats['bin'].apply(lambda b: b.mid)

fig, ax = plt.subplots(figsize=(10, 6), facecolor=BG)
ax.set_facecolor(PANEL)
ax.tick_params(colors=GREY)
for spine in ax.spines.values():
    spine.set_edgecolor(GREY)
ax.grid(True, color=GREY, alpha=0.2, linewidth=0.5)

ax.bar(bin_mid, bin_stats['mean'], width=3.2, color=BLUE, alpha=0.7, label='Bin mean')
ax.errorbar(bin_mid, bin_stats['mean'], yerr=bin_stats['std'],
            fmt='none', color=FG, capsize=4, lw=1.5, label='±1σ')

y_fit_bins = slope_amb * np.array(bin_mid) + intercept_amb
ax.plot(bin_mid, y_fit_bins, color=YELLOW, lw=2, ls='--', label='Linear fit')

ax.set_xlabel('Avg BME ambient (°C)', color=FG)
ax.set_ylabel('raw_m mean (m)', color=FG)
ax.set_title('Per-bin mean ± σ (bins ≥20 samples, 4°C wide)', color=FG, fontsize=12)
ax.legend(facecolor=PANEL, edgecolor=GREY, labelcolor=FG, fontsize=9)

plt.tight_layout()
plt.savefig(f'{OUT}/fig3_bins.png', dpi=150, bbox_inches='tight', facecolor=BG)
plt.close()
print("Saved fig3_bins.png")

# ── Summary ───────────────────────────────────────────────────────────────────
print(f"""
════════════════════════════════════════════════════════════
  OPERATION ICEBOX — RESULTS SUMMARY
════════════════════════════════════════════════════════════
  Samples analysed : {len(df):,}
  Ambient range    : {df['avg_amb'].min():.1f} – {df['avg_amb'].max():.1f} °C
  Die range        : {df['avg_die'].min():.1f} – {df['avg_die'].max():.1f} °C

  LINEAR FIT (avg BME ambient):
    Coefficient    : {slope_amb:+.4f} m/°C
    R²             : {r_amb**2:.4f}
    p-value        : {p_amb:.2e}

  MULTIVARIATE FIT (alpha_amb + chimp_amb separately):
    Alpha coeff    : {coef_alpha:+.4f} m/°C
    Chimp coeff    : {coef_chimp:+.4f} m/°C
    R²             : {r2_multi:.4f}

  FIRMWARE CORRECTION CONSTANTS:
    TEMP_COEFF     : {-slope_amb:+.4f}   // m/°C (sign inverted — correction opposes drift)
    CAL_AMB_C      : 31.3               // BME ambient at calibration time (°C)

  At 40°C ambient vs 31.3°C cal:  correction = {-slope_amb * (40 - 31.3):+.3f} m
════════════════════════════════════════════════════════════
""")
