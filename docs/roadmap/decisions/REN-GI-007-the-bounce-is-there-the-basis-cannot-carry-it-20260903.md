# REN-GI-007 — The sun bounce is not missing; the six-face basis cannot carry it to a wall

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** REN-GI-006 (the gain is inert on a shaded wall), REN-GI-002 (the sun proxy)

## The question

REN-GI-006 measured twelve times the sun bounce moving a shaded wall by 0.1%, and named the next
step as a *diagnosis* rather than another tuning pass: is the bounce **absent** on a vertical
surface, or is it **present and stored where a vertical normal cannot read it**?

## The instrument

`WGR_GI_RSM_ISO=1` spreads each RSM sample's radiance evenly over all six ambient-cube faces
instead of by the direction it arrived from. Same energy, no direction. Diagnostic only, off by
default; it is not a shipping path and is not offered in the dev panel.

## Measured (Everon, 17:00, 3441×1440, `POSEIDON_VSYNC=0`)

| | shaded wall (`gi_shade270`) | town lower frame (`perf_town`) |
|---|---:|---:|
| bounce off (gain 0) | 62.44 | 62.59 |
| gain 12, directional (shipped) | 62.52 (+0.1%) | **64.53 (+3.1%)** |
| gain 12, isotropic (diagnostic) | **64.98 (+4.1%)** | 63.64 (+1.7%) |

**The energy is there.** Two and a half luma of it reach the wall the moment the direction is
discarded. So the proxy renders, the probes gather it, and the gain scales it; nothing in that
chain is broken, which is what REN-GI-006 could not distinguish.

**And the directional store is not simply wrong.** On the town it beats the isotropic version
almost two to one (+3.1% against +1.7%) — spreading the energy is a loss there, exactly as it
should be, because direction is information.

## What this actually says

A six-face ambient cube reconstructs irradiance as Σ E_face · n_component². For a bounce that
arrives from below — which is what a probe standing over sunlit ground sees, the near field
dominating by 1/d² — the energy lands in the down face, and a horizontal normal multiplies that
face by ≈ 0. That is *correct* for a delta from straight below; a wall really does receive
almost nothing from the ground immediately beneath it. What the basis cannot express is the
broad, partly-horizontal arrival that a real wall gets from ground several metres out in front
of it: with six faces those directions collapse onto the same face as the near field.

The town measures better because its probes sit between buildings and gather bright ground over
a wide span at similar height, so `back` keeps a real horizontal component. The shade pose sits
over a hillside where the near field is directly below. **The difference is scene geometry
meeting the basis, not a defect in the proxy.**

## The named next step, and what is NOT being done

The fix for walls is a **directional basis that can carry it** — second-order spherical
harmonics (9 coefficients) in place of the 6-face cube for the probe radiance — not another
gain, and not the isotropic path, which trades the town away to buy the wall.

That is a change to the probe storage format, the compute shader and the gather, i.e. the whole
of REN-GI-001/003 re-fitted, and it is **not** started here. It is written down so the next
session does not repeat the tuning attempt this decision closes.

Defaults are unchanged: directional store, gain 1.
