
"GRIN" is gradient-index (GRadient-INdex): a medium whose refractive index varies smoothly through space instead of jumping at sharp boundaries the way it does at, say, a glass surface. A "layered density" GRIN model is the standard way to *handle* such a medium computationally — you approximate the continuous variation as a stack of thin, homogeneous layers, each with its own constant index, and apply Snell's law at every interface. Make the layers thin enough and the staircase converges to the true smooth ray path.

**Why density drives the index**

For most transparent media the refractive index tracks density almost linearly. The Gladstone–Dale relation says (n − 1) is proportional to density; the Lorentz–Lorenz relation is the more exact version. So wherever the material is denser, n is higher. A density profile *is* an index profile, which is why "GRIN model" and "layered density model" get used for the same thing — the layers are really layers of differing density, and the index follows.

**What the layered model computes**

In each homogeneous slab a ray travels straight; at each boundary it refracts per n₁sin θ₁ = n₂sin θ₂. For a medium stratified in one direction (index a function of height z only), this chains into a conserved quantity along the ray — Bouguer's invariant, n(z)·sin θ(z) = constant — which is just Snell's law surviving across all the layers at once. In the limit of infinitely thin layers you recover the continuous ray equation, d/ds(n · dr/ds) = ∇n. The practical upshot: **rays bend toward the higher-index (denser) region**, curving rather than running straight.

Where this shows up: graded-index optical fibers and Luneburg lenses (engineered GRIN), mirages and atmospheric refraction (natural GRIN from air density falling with altitude), ocean acoustics (the SOFAR sound channel), and seismology (wave speed rising with depth).

**Why this is relevant to your 60 km ranging link**

This is almost certainly the connection to your earlier atmospheric question. Your RF path crosses an atmosphere whose density — and therefore radio refractive index — decreases with altitude, making the lower atmosphere a natural GRIN medium. Two consequences for a time-of-flight ranging measurement:

The path bends. The beam curves gently downward following the density gradient, so it reaches slightly beyond the geometric horizon — this is exactly what the "4/3 Earth radius" (k-factor) approximation in radio path planning encodes, and it's a coarse one-parameter stand-in for a full layered-density model. It's also part of why your mountaintop link clears terrain better than a flat-earth calculation suggests.

The path slows. Because n > 1, the signal travels slightly slower than c, and the cumulative delay over 60 km adds a small positive bias to your measured distance. A layered atmospheric model is precisely how you'd correct it: integrate the refractivity through the layers to get the excess delay. For 2.4 GHz the dominant term is tropospheric refractivity (the familiar N-units, ~300 at sea level, driven by pressure, temperature, and humidity), and over 60 km the wet component especially can shift your reading by an amount that matters once you're calibrating to sub-meter precision.

So if you want your ranging numbers to hold up across weather and seasons, the surveyed-distance calibration we discussed handles the *static* offset, but a layered-density (GRIN) refractivity model is what accounts for the *varying* atmospheric part on top of it.

Is this in service of refraction-correcting the ranging link, or are you looking at GRIN in a different context (optics, a Luneburg-style antenna, something else)? I can go deeper on whichever.

## Grin code description:

Good use case — and the right framing matters, so here's the key insight first: the troposphere adds a real, sizeable bias to your link. Over 60 km the excess path is roughly **15 m (~50 ns one-way)**, and the *weather-driven variation* in that is order ~1 m — right at the sub-meter precision you were targeting. So this is worth modeling.

**The physics that makes the algorithm simple**

The excess delay splits into two parts: the signal *slowing* because n > 1 (refractivity term), and the ray *bending* into a longer curved path. For a near-line-of-sight link the bending term works out to only a few centimeters of extra length — negligible — so you can skip the hard boundary-value ray-trace entirely. The whole correction collapses to a single line integral of refractivity along the (curved-earth) straight path:

ΔL = 10⁻⁶ ∫ N(h) ds,  where N = (n−1)×10⁶

So the algorithm is: lay the chord between your two antennas through a layered atmosphere, sample N at each step's height, integrate. That's it.

**The refractivity model**

N comes from surface weather via ITU-R P.453: a dry term (77.6·P/T) and a wet term (3.73×10⁵·e/T²), where e is water-vapor pressure. The dry part decays slowly with height (~8 km scale height), the wet part fast (~2 km). You evaluate that profile at each sample height along the path.

Here's a self-contained implementation you can drop into the lora-ranger:**How it plugs into what you already built**

The important subtlety: your surveyed-distance calibration *already* absorbs the atmosphere as it was on calibration day. So you don't subtract the full ~15 m bias — that would double-count. Instead you store the excess computed under calibration-day weather as a reference, and at each measurement apply only the *change* since then (the `dynamic_corr` in the file's footer). That's the weather-adaptive part, and it's where the sub-meter wins live.

This pairs naturally with your fixed-station hardware: drop a **BME280** (pressure/temp/humidity) at each end, feed live readings into `excess_delay()`, and the correction tracks weather automatically. Both ends fixed means heights, ground distance, and reference altitude are all constants you set once.

**Accuracy and limits to know**

The exponential profile with fixed scale heights is the standard engineering approximation, good to a meter or so in normal conditions — the two scale-height constants are the main knobs if you want to tune against observed data. For higher fidelity you'd replace `refractivity_at()` with a measured radiosonde or NWP profile, which the code is structured to let you swap in cleanly.

The one regime where this model genuinely breaks is **ducting / super-refraction** — strong temperature inversions or a path skimming over water can bend the signal far more than a smooth exponential predicts. Your mountaintop-to-valley path could cross an inversion layer on calm mornings, so if you see ranging readings jump anomalously under stable cool conditions, that's the likely cause and it's a known limit of any simple stratified model, not a bug in the integration.

Run the included test (`cargo test -- --nocheck` or just call it) with your real heights and distance to see your specific bias number before wiring it into the correction path. Want me to add the optional ray-bending term too, or a quick routine that ingests a radiosonde profile array instead of the exponential?

'''//! Tropospheric (GRIN) excess-delay model for an SX1280 time-of-flight ranging link.
//!
//! The lower atmosphere is a gradient-index medium: refractive index falls with
//! height as air density drops. That makes the radio signal travel slightly slower
//! than c, adding excess electrical path length that the SX1280 reports as extra
//! distance. This module projects that excess so you can subtract it.
//!
//! Dominant term  = refractivity slowing:  dL = 1e-6 * integral( N(h) ds )
//! Omitted term   = ray bending (curved path is longer): ~cm over 60 km, negligible.
//!
//! no_std note: replace the `.exp()` calls with `libm::exp` if building without std.

const C_VAC: f64 = 299_792_458.0; // speed of light in vacuum, m/s
const RE: f64 = 6_371_000.0;      // mean Earth radius, m

/// Saturation water-vapour pressure over water [hPa], Buck (1981). `t_c` in degC.
fn sat_vapour_pressure(t_c: f64) -> f64 {
    6.1121 * ((17.502 * t_c) / (t_c + 240.97)).exp()
}

/// Surface refractivity split into dry and wet parts (ITU-R P.453).
/// `p_hpa` total pressure [hPa], `t_k` temperature [K], `rh` relative humidity [0..1].
/// Returns (N_dry0, N_wet0) in N-units at the sensor's altitude.
fn surface_refractivity(p_hpa: f64, t_k: f64, rh: f64) -> (f64, f64) {
    let t_c = t_k - 273.15;
    let e = rh * sat_vapour_pressure(t_c);      // vapour partial pressure [hPa]
    let n_dry = 77.6 * p_hpa / t_k;
    let n_wet = 3.73e5 * e / (t_k * t_k);
    (n_dry, n_wet)
}

/// Refractivity N at mean-sea-level height `h_msl` [m], two-component exponential
/// decay referenced to the sensor altitude `h_ref_msl`. Scale heights are typical
/// lower-troposphere values and are tunable; swap in a radiosonde/NWP profile here
/// for best accuracy.
fn refractivity_at(h_msl: f64, h_ref_msl: f64, n_dry0: f64, n_wet0: f64) -> f64 {
    const H_DRY: f64 = 8_000.0; // dry scale height [m]
    const H_WET: f64 = 2_000.0; // wet scale height [m]
    let dh = h_msl - h_ref_msl;
    n_dry0 * (-dh / H_DRY).exp() + n_wet0 * (-dh / H_WET).exp()
}

/// Output of the excess-delay computation (all one-way).
#[derive(Clone, Copy, Debug)]
pub struct ExcessDelay {
    pub excess_path_m: f64, // extra electrical length == range bias to subtract [m]
    pub delay_ns: f64,      // excess propagation delay [ns]
    pub chord_m: f64,       // straight-line geometric distance between antennas [m]
}

/// Compute tropospheric excess path/delay for a fixed point-to-point link.
///
/// * `h1`, `h2`      antenna heights above mean sea level [m]
/// * `ground_dist_m` surface arc distance between the two sites [m]
/// * `p_hpa`,`t_k`,`rh` surface weather measured at altitude `h_ref_msl`
/// * `segments`      integration steps along the path (200 is plenty)
pub fn excess_delay(
    h1: f64,
    h2: f64,
    ground_dist_m: f64,
    p_hpa: f64,
    t_k: f64,
    rh: f64,
    h_ref_msl: f64,
    segments: usize,
) -> ExcessDelay {
    let (n_dry0, n_wet0) = surface_refractivity(p_hpa, t_k, rh);

    // Endpoints in a 2-D Earth-centred plane (curvature kept in the geometry).
    let r1 = RE + h1;
    let r2 = RE + h2;
    let alpha = ground_dist_m / RE; // subtended central angle [rad]
    let (x1, y1) = (r1, 0.0);
    let (x2, y2) = (r2 * alpha.cos(), r2 * alpha.sin());

    let chord = ((x2 - x1).powi(2) + (y2 - y1).powi(2)).sqrt();
    let ds = chord / segments as f64;

    // Trapezoidal integration of N along the chord.
    let mut sum_n = 0.0;
    for i in 0..=segments {
        let t = i as f64 / segments as f64;
        let x = x1 + t * (x2 - x1);
        let y = y1 + t * (y2 - y1);
        let h_msl = (x * x + y * y).sqrt() - RE;
        let n = refractivity_at(h_msl, h_ref_msl, n_dry0, n_wet0);
        let w = if i == 0 || i == segments { 0.5 } else { 1.0 };
        sum_n += w * n;
    }

    let excess_path_m = 1e-6 * sum_n * ds; // = integral( (n-1) ds )
    let delay_ns = excess_path_m / C_VAC * 1e9;

    ExcessDelay { excess_path_m, delay_ns, chord_m: chord }
}

// ---------------------------------------------------------------------------
// Folding it into your surveyed-distance calibration
// ---------------------------------------------------------------------------
// Your one-time surveyed-distance offset already absorbs the atmosphere as it
// was on calibration day. So in service apply only the CHANGE since then:
//
//   // once, on the day you calibrated against the known site-to-site distance:
//   let ref_excess = excess_delay(h1, h2, d, p_cal, t_cal, rh_cal, h_ref, 200).excess_path_m;
//
//   // each measurement, with live weather from a BME280 at the station:
//   let now = excess_delay(h1, h2, d, p_now, t_now, rh_now, h_ref, 200).excess_path_m;
//   let dynamic_corr = now - ref_excess;                 // metres
//   let true_distance = measured - survey_offset - dynamic_corr;
//
// If you did NOT do a surveyed calibration, subtract the absolute bias instead:
//   let true_distance = measured - now;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sixty_km_mountain_link() {
        // Mountaintop at 1500 m to a station at 300 m, 60 km apart, mild damp air.
        let r = excess_delay(1500.0, 300.0, 60_000.0, 850.0, 283.15, 0.6, 1500.0, 200);
        println!(
            "chord = {:.1} m, excess = {:.2} m, delay = {:.1} ns",
            r.chord_m, r.excess_path_m, r.delay_ns
        );
        assert!(r.excess_path_m > 5.0 && r.excess_path_m < 30.0);
    }
}'''
