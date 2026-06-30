Here's the actual link budget, anchored to the 40 km field result we already have so it's not just theory.

**Path loss at 60 km**

Free-space path loss at 2.45 GHz over 60 km is about **136 dB** (FSPL = 20·log₁₀ d + 20·log₁₀ f + 92.45). That's the number your antennas have to overcome.

**System sensitivity, back-calculated from real data**

Stuart's link still worked at 40 km with 2 dBi antennas at both ends down to 4 dBm transmit, at SF10/BW406. Plugging that threshold into the budget gives an effective system sensitivity of about **−124 dBm** at those ranging settings — a realistic, real-world figure that already bakes in cable and miscellaneous losses. Going from 40 to 60 km only adds ~3.5 dB of path loss, so you're not far off.

**The budget (non-PA module, 12 dBm, symmetric antennas)**

| Line item | Value |
|---|---|
| Tx power (M12S / T3-S3, no PA) | +12 dBm |
| Antenna gain ×2 | +2G dB |
| Free-space loss @ 60 km | −136 dB |
| Cable + connector + pointing/polarization | −4 dB |
| Received power | 2G − 128 dBm |
| System sensitivity (SF10/BW406) | −124 dBm |
| **Link margin** | **2G − 4 dB** |

**What that means for antenna gain (per end)**

| Antenna per end | Margin | Verdict |
|---|---|---|
| ~9 dBi | ~14 dB | Closes, thin for weather |
| ~12 dBi | ~20 dB | Solid minimum for a permanent link |
| **16–19 dBi panel** | **~28–34 dB** | **Recommended sweet spot** |
| 24 dBi grid/dish | ~44 dB | Bombproof, overkill but cheap |

So the honest answer: you only *need* about **12 dBi each end** to close 60 km with a healthy 20 dB margin — far less than I'd implied earlier. A pair of **16–19 dBi 2.4 GHz panel antennas** is the practical recommendation: generous margin, and their ~18–25° beamwidth makes aiming forgiving. Step up to **24 dBi grids** only if you want maximum robustness — their ~8–10° beam is narrow, but since both ends are fixed you aim once (peak it using the SX1280's RSSI reading) and lock it down. Mount rigidly so wind doesn't drift the aim.

**Two levers and one caution**

A higher spreading factor is free margin: SF12/BW203 buys roughly +8 dB of sensitivity over the SF10/BW406 ranging baseline, which would cut your antenna requirement by ~4 dBi per end — at the cost of slower ranging exchanges. SF10/BW406 is the proven-for-ranging setting, so treat higher SF as a tunable if you want to shrink the antennas.

The caution is EIRP again: 12 dBm into a 24 dBi grid is 36 dBm (4 W) EIRP. Under ISED RSS-247 in BC that may be at or over the fixed point-to-point ceiling, so the low conducted power of the non-PA module actually helps you here — but verify the legal limit before you settle on grid antennas. Panels at 16–19 dBi keep you at a more comfortable 28–31 dBm EIRP.

Want me to pull up specific current 2.4 GHz panel and grid antennas (with connector type matched to your T3-S3's SMA) so you have actual models to order?

Here are real, currently-available models in both classes, plus the connector issue you need to plan around.

**First — the connector reality**

Your T3-S3 has a plain **SMA** jack (the LoRa-board standard). But nearly every off-the-shelf 2.4 GHz directional antenna uses one of two *different* connectors: WiFi-style **RP-SMA** (panels/yagis) or **N-female** (grids). Neither mates directly with your board. So budget for one pigtail per antenna: an **N-male → SMA-male** jumper (LMR-240/400) for grids, or an **RP-SMA → SMA adapter** for WiFi panels. Keep that jumper short — at 2.4 GHz, LMR-240 loses ~0.4 dB/ft, so a 3 ft run already eats ~1.2 dB. Mount the radio right at the antenna.

**Panel antennas (16–19 dBi) — the recommended sweet spot**

These give you ~20–28 dB margin with a forgiving 18–25° beam, easy to aim and lock once.

- **Tupavco TP511** — 2.4 GHz, 20 dBi, ~18° beamwidth, N-female, weatherproof with U-bolt mast mount. A well-known, reasonably priced workhorse for fixed point-to-point. It allows horizontal or vertical polarization and comes with two U-bolts for pole mounting; note it's N-female, so you'll need the pigtail to your device connector.
- **19 dBi 2.4 GHz outdoor panel** (sold by Rfwel and others) — available in either **SMA-female** or N-female versions. The SMA-female variant is the cleanest match to your board (just an SMA-male–SMA-male jumper). Sold as a 19 dBi 2.4 GHz Wi-Fi outdoor panel antenna.

**Grid/parabolic antennas (24 dBi) — maximum robustness**

Overkill on margin (~40+ dB) but cheap insurance; the tradeoff is a narrow ~8–14° beam that demands careful one-time aiming. All are N-female.

- **Altelix 2.4 GHz 24 dBi grid parabolic** — directional grid parabolic, N-female connector, weather-resistant, explicitly sold for 2.4 GHz point-to-point.
- **Laird 24 dBi parabolic grid** — commercial-grade; 8° vertical / 10° horizontal beamwidth, ships with a 30" N-female pigtail and mount included. This is the most professionally built of the bunch. Laird also makes a **19 dBi** version (11°/17° beamwidth) that's a nice middle ground — easier to aim than the 24 dBi, still huge margin.
- **Premiertek / PolarisUSA / Simple WiFi 24 dBi grids** — budget options, all N-female, ~8° beam. PolarisUSA rates roughly 56 km of range at the lowest/most-robust data rate, which lines up with where you're operating.

**My pick for your build**

Go with a matched pair of **19 dBi units** — either the Laird 19 dBi grid or a 19 dBi SMA-female panel. That hits the ~28 dB margin band I'd want for a permanent all-weather link, while keeping the beam wide enough (11–17°) that aiming is straightforward and small wind-induced mast flex won't drop the link. Reserve the 24 dBi grids for if you later push past 60 km or want to drop transmit power for EIRP headroom — their ~8° beam is less forgiving on a tall mast.

Whatever you pick, get the **same antenna at both ends**, match polarization (both vertical or both horizontal), and peak the aim using the SX1280's RSSI as a live signal meter during install.

One EIRP note to close the loop: 12 dBm into a 19 dBi panel is ~31 dBm (1.3 W) EIRP, and into a 24 dBi grid ~36 dBm (4 W). Under ISED RSS-247 in BC the fixed point-to-point allowance is generous, but the 24 dBi case is where you'd want to verify you're inside the limit — another reason the 19 dBi pick is the comfortable choice.

Want me to put together the full per-site parts list (antenna + pigtail + adapter + mast hardware + the BME280 for the delay model) as a single ordering checklist?


