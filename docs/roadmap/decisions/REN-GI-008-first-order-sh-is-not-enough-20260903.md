# REN-GI-008 — First-order SH is not enough: it helps the room and hurts the wall

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** REN-GI-007 (the six-face basis cannot carry the bounce to a wall)

REN-GI-007 named second-order SH as the fix. This decision built the **first-order** version
first, because it is four coefficients against nine and costs one extra texture load per probe
corner instead of six. It does not work, and that is worth recording so nobody builds it twice.

## What was built

`WGR_GI_BASIS=sh` stores probe radiance as L0 plus the three L1 coefficients in slabs 0..3 of the
existing radiance texture — same texture, same bind group, same state buffer; only the meaning of
the first four slabs changes, and slabs 4/5 are cleared so a stale cube cannot be read by
mistake. The gather reconstructs irradiance/π as `0.2820948·L0 + 0.3257350·(L1·n)`. The compute
shader accumulates both bases from the same rays and the same RSM samples, so one dispatch and
one binary carry both. `cube` remains the default.

The frame uniform's `gi.w` lane, which used to carry a GI debug flag that no shader read, carries
the basis now.

## Measured (Everon, 3441×1440, `POSEIDON_VSYNC=0`)

| pose / region | cube | SH-1 | |
|---|---:|---:|---|
| `gi_house270` shaded interior wall | 56.01 | 57.47 | **+2.6%** |
| `gi_house270` sunlit wall at the window | 112.75 | 112.18 | −0.5% |
| `gi_shade270` shaded stone wall | 62.44 | **59.37** | **−4.9%** |
| `perf_combat` whole frame | 129.86 | 129.86 | 0.0% |

And the test the basis was built for — does the sun bounce now reach the wall:

| | gain 0 | gain 12 | |
|---|---:|---:|---|
| cube | 62.44 | 62.52 | +0.1% |
| **SH-1** | 59.45 | **58.46** | **−1.7%** |

**With SH-1 the bounce makes the wall darker.** Cost is unchanged either way (colour sub-pass
4.04 → 4.17 indoors, 6.07 → 5.81 on `perf_combat`, both inside the run spread).

## Why, and what it says about the next step

Four coefficients cannot hold a sky-dominated field *and* a directional bounce at once. The
reconstruction is a single linear lobe: pulling it toward the ground bounce takes the same lobe
away from the sky that supplies most of the light, and the clamp at zero that keeps SH-1 from
going negative removes the rest. The result is worse than the cube's six independent, always
non-negative face averages.

So REN-GI-007's recommendation stands **exactly as it was written — second order, nine
coefficients** — and this decision has narrowed it: the cheap version is not a stepping stone,
it is a dead end, because the failure is the basis's *order*, not its plumbing. The plumbing
built here (dual-basis compute, the `gi.w` lane, the slab reuse) is what SH-2 will extend.

## Default

`cube`. `WGR_GI_BASIS=sh` is kept as a measured, documented negative result, not as an option to
recommend.
