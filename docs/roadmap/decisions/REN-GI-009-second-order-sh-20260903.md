# REN-GI-009 — Second-order SH: every shaded surface measured comes up, no lit surface moves

**Date:** 2026-09-03
**Branch:** `asset-obs-001`
**Follows:** REN-GI-007 (the six-face basis loses it), REN-GI-008 (first order is not enough)

## What was built

`WGR_GI_BASIS=sh2` stores probe radiance as **nine second-order SH coefficients** instead of six
ambient-cube faces. Both probe textures are nine slabs tall now (the moments use the first six of
their own; cutting both to one height costs about half a megabyte and lets one copy extent cover
them), and the state buffer is 15 vec4 per probe: 9 radiance + 6 moments.

The gather reconstructs irradiance/π as Σ (Â_l/π)·L_lm·Y_lm(n), with Â0/π = 1, Â1/π = ⅔ and
Â2/π = ¼ — nine texture loads a probe corner against the cube's three. One dispatch accumulates
all three bases from the same rays and the same RSM samples, so one binary carries `cube`,
`sh` and `sh2` and every comparison below is one env var apart.

## Measured (Everon, 3441×1440, `POSEIDON_VSYNC=0`)

| pose / region | cube | SH-1 | **SH-2** |
|---|---:|---:|---:|
| `gi_house270` shaded interior wall | 56.01 | 57.47 (+2.6%) | **57.98 (+3.5%)** |
| `gi_house270` sunlit wall at the window | 112.74 | 112.18 (−0.5%) | **112.50 (−0.2%)** |
| `gi_shade270` shaded stone wall | 62.44 | 59.37 (−4.9%) | **63.18 (+1.2%)** |
| `perf_combat` whole frame | 129.80 | 129.86 (0.0%) | **129.74 (−0.0%)** |

Cost: the colour sub-pass goes 4.08 → 4.32 ms indoors and 5.69 → 5.88 on `perf_combat`, i.e.
**about +0.2 ms**, and the probe update is unchanged at 0.16–0.20 ms.

SH-1's outdoor regression is gone: one linear lobe could not hold a sky-dominated field and a
directional term at once, and the second band can.

## The bounce still does not reach that wall — and that is not the basis

| gain 0 → gain 12 on `gi_shade270` | |
|---|---:|
| cube | +0.1% |
| SH-1 | −1.7% |
| SH-2 | −0.1% |

So REN-GI-007's diagnosis was **half right**. The basis *was* losing light on a vertical
surface — SH-2 recovers +1.2% there from the ray field — but the RSM term specifically is still
zero on that wall, and the reason is in the gather, not the storage: the ground directly beneath
those probes is *in the building's own shadow* at 17:00, so `cos_sun <= 0` skips it, and the
sunlit grass further out is killed by 1/d². A wall whose adjacent ground is also in shadow really
does receive almost no first bounce. The sun proxy is behaving.

## Default: unchanged, pending the owner's look

`cube` remains the default and `sh2` is opt-in **only because this is a picture change to a
feature that is on by default**, and the owner looks at those. On the measurements there is no
argument for the cube: SH-2 is better on every shaded surface and identical on every lit one.
