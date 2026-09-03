# REN-SKY-004 — The water reflection marched the sky's 128-step clouds; it gets its own 32-step pipeline

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** REN-SKY-001/002 (the sky's own step count, which the owner chose at 128 and which
this decision does **not** touch)

## What was found

The GPU frame was re-attributed on the current binary, and on `perf_water` the planar
reflection block came to **2.36 ms of a 30.98 ms frame (7.6%)** — of which `Planar: clouds`
alone was 1.31. On the open-sea pose added below it is **2.55 ms**.

That pass called the *same* cloud pipeline as the sky: 128 primary steps. The reflection cannot
show that work. Its result is written to a half-resolution buffer, reflected off a displaced FFT
surface, and read back through the roughness mip chain.

## What changed

A second cloud pipeline, `wgr_cloud_reflection`, compiled from the same shader with
`CLOUD_STEPS = 32`, used only by the planar reflection pass. The sky keeps its 128-step pipeline
untouched — this is a different pipeline object, not a lowered setting.

`WGR_CLOUD_STEPS_REFLECTION=<8..256>` overrides it; setting it to 128 restores the old
behaviour exactly, which is how the A/B below was run on one binary.

## Measured (Everon, noon, 3441×1440, `POSEIDON_VSYNC=0`, two alternating runs per arm)

| | reflection 128 | reflection 32 |
|---|---:|---:|
| **open sea** `Planar: clouds` | 2.55 ms | **1.20 ms** |
| open sea, GPU frame | 30.59 | 30.06 |
| open sea, CPU frame | 31.17 | 29.46 |
| `perf_water` `Planar: clouds` | 1.17 | **0.46** |
| `perf_water`, GPU frame | 28.97 | 28.20 |
| `Sky: cloud march` (control) | 5.11 | 5.12 |

The sky's own march does not move, which is the check that the second pipeline went only where
it was meant to.

## The picture

Two poses, both looked at rather than only differenced:

- `perf_water` (distant sea, grass foreground) — indistinguishable.
- **the open-sea pose** (`perf_shore_9200.0.eden`, new: the same soldier moved east to 9200 /
  6500, standing in the water with reflected cloud streaks filling the lower two thirds) — the
  reflected cloud streaks are soft and low-contrast in both arms. No grain, no banding, no
  stepping in the 32-step arm.

A pixel metric could not settle this and is reported so it is not mistaken for evidence: the sea
never repeats between runs, so the **control** (same setting, two runs) already differs by mean
5.94 over the water region while the arm differs by 6.15. The judgement here is the eye's, on
the crops, not the metric's.

## Also measured, and not shipped: the FFT cascades

`WGR_WATER_FFT_CASCADES=<1..4>` caps how many spectrum cascades are live (it disables them, so
the picture is honest; skipping a dispatch would leave stale displacement in the array layer).
Default is unchanged.

4 → 2 cascades on `perf_water`: the FFT block itself only falls 0.83 → 0.59 ms — but **`Water
draw` falls 6.21 → 4.32**. The cost of a cascade is in the *surface shader* sampling it, not in
the simulation. So this is a quality lever worth ~2.1 ms on a water scene, not a free win, and
the default stays at what the preset asks for. The distance-based cascade weighting (WTR-031/032)
already zeroes cascades whose texels go sub-pixel, so there is nothing to reclaim at range.

## Not done

Whether 64 would be a safer default than 32 was not tested; 32 showed nothing to fix.
