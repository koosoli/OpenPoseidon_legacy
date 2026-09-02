# REN-SKY-001 — The cloud march at 64 steps: the A/B the step-count tier never had

**Date:** 2026-09-02
**Branch:** `asset-obs-001`
**Follows:** ARCH-001's frame table (sky cloud march 1.31 ms / 11.7% on `perf_combat`),
`HANDOVER-20260824` (the step-count tier, shipped without an A/B since the SH fix)

## Measured

`perf_combat`, bench window size, RTX 3070, three runs per arm, binary `9ef3e84a`,
`WGR_CLOUD_STEPS` as the only difference:

| steps | Sky: cloud march (mean / min) | GPU frame (mean) | CPU frame avg | p95 |
|---:|---:|---:|---:|---:|
| 128 (old default) | 2.915 / 2.867 ms | 25.47 | 29.17 | 32.65 |
| **64** | **1.851 / 1.808** | 24.38 | 28.66 | 32.09 |
| 32 | 1.241 / 1.233 | 23.95 | 28.39 | 31.15 |

The march scales a little better than linearly with the step count (the light march, 4
steps, and the composite, 0.11 ms, do not move). At this window it is 2.9 ms, not ARCH-001's
1.3: the pass is per-pixel, and the bench window has ~2x the pixels of that capture. The
CPU frame moves less than the march does because the scene is GPU-bound only in part.

## The picture

Cloud shapes differ between arms because the clouds evolve with time between runs, so there
is no pixel diff; the crops (`.tmp-sky/sky_zoom_128_64_32.png`) are judged by eye. To my eye
128 and 64 were indistinguishable at full resolution; the owner told them apart at once and
preferred 128 (see Decision). 32 shows visible grain along cloud edges — the march's dither
(`hash21`) is no longer hidden by enough samples.

## Decision

**Default stays 128.** 64 shipped as the default for one build (`1ae6f40e`); shown the three
crops without labels, the owner preferred the 128 picture on sight — the eye sees what the
capture crops did not resolve. `WGR_CLOUD_STEPS=64` is the lever for a slow GPU (~1 ms of
GPU per frame at this window); 32 shows grain and is not recommended.

## Not measured

One machine, one time of day (clear-ish sky with scattered cumulus). Dense overcast or
sunset, where the march works hardest and the dither would show most, is the case to check
before recommending 64 even as a slow-GPU lever; DLSS's temporal accumulation should hide grain the
capture (DLSS off) does not, which makes the capture the pessimistic case.
