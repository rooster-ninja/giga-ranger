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
