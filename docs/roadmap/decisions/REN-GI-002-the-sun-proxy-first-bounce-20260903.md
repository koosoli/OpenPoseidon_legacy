# REN-GI-002 — The sun proxy: the first bounce, with colour

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** REN-GI-001 (the irradiance probe volume)

## What it adds

Stage 1's probes could see the terrain (a heightfield march) and roofs (the interior-sky dome
maps), but nothing beside them and nothing with colour: the dome maps carry occlusion, not
albedo, so a sunlit red wall threw neutral light. Stage 2 gives the probes a **reflective
shadow map**: the GPU-driven depth path draws the retained set once from the sun over the
volume's box into three targets —

| target | contents |
|---|---|
| depth (Depth32Float) | where the sunlit surface is; the probe integrator unprojects it |
| albedo (Rgba8Unorm) | texture × material diffuse; alpha 1 = something was drawn |
| normal (Rgba8Unorm) | the world normal, `n * 0.5 + 0.5`, flipped for back faces |

1024² over a 96 m box is 9.4 cm per texel. Each probe projects into the map and gathers a
Vogel disc of texels around itself (16 by default, 12 m radius): each is a surface element of
area `πr²/n` whose radiance toward the probe is `albedo × sun × cos(θ_sun) / π`, weighted by
`area·cos(θ_q)/d²` and split over the ambient cube's six faces. Walls, trees and vehicles all
contribute, because the sun sees them.

## Budgeted, not event-driven

The proxy re-renders when its **view** moves (the camera leaves its probe cell, or the sun
turns a quarter degree) and otherwise at most **once every 8 frames** when the retained set
changed. Keying it on the instance epoch alone was the first version and would have re-rendered
every frame on any scene with a walking soldier — every instance update bumps that epoch.

The proxy is also only marked rendered by the **draw site**, not by `prepare`. The first
version marked it in `prepare`, so the single draw landed on frame 0 — whose world was still
empty — and the proxy then held nothing but its clear colour for the whole run. The gate log
(`GI sun proxy did not draw: <reason>` / `GI sun proxy drawn (redraw n)`) is what found it, and
it stays in.

## Measured (Everon `perf_town`, 3441×1440, VSync off, one run per arm)

| arm | 17:00 CPU frame / GPU | 12:00 CPU frame / GPU |
|---|---|---|
| GI off | 17.96 / 18.4 | 18.76 / 18.9 |
| stage 1 (sky + terrain probes) | 18.32 / 19.2 | 18.98 / 19.4 |
| **+ sun proxy** | 18.98 / 18.9 | 19.13 / 15.2 |

Probe update 0.10–0.16 ms. The proxy's own pass does not appear in the capture's timer column
because it runs on perhaps one frame in eight and the capture frame is usually not one of them.

**The bounce is real and small.** Stage 2 against stage 1 differs in 0.03% of pixels at 17:00
and 0.00% at noon. Raising the gain to 12× moves 3.8% of pixels by more than 8 levels and the
ground's mean luma from 61.5 to 62.9 (+2.3%), so the term is present and physically scaled, not
broken. It is small on these poses for a good reason: the bounce arrives from below and from the
side, so it lands on the ambient-cube faces that a mostly-upward-facing scene barely samples,
and the sky term dominates those faces anyway. The scene that should show it — deep shade beside
a big sunlit wall — is not in the bench set.

## Switches

`WGR_GI_RSM=0` turns the bounce off (stage 1 alone), `=<n>` sets the sample count;
`WGR_GI_RSM_GAIN=<x>` scales it. The dev panel's GI tab has samples, radius and gain.
`WGR_GI=0` still turns the whole volume off.

## Next

Per-probe visibility (Chebyshev, DDGI-style) against leaks, and a pose that actually tests a
coloured bounce before the gain default is chosen.
