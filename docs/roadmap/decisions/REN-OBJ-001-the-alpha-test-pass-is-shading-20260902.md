# REN-OBJ-001 — The alpha-tested object pass is shading, not geometry; foliage stops sampling the screen-space occluders

**Date:** 2026-09-02 (night)
**Branch:** `asset-obs-001`
**Follows:** the roadmap's third named target after AUD-001 (`Objects: colour alpha-test`,
1.0–1.1 ms then, 2.4 ms at the bench window used tonight)

## The census, by capture A/B (Everon `perf_combat`, bench window, RTX 3070, one run per arm)

| arm | colour alpha-test | colour solid | note |
|---|---:|---:|---|
| reference | 2.40 ms | 0.36 | 0.11 M triangles, 61 draws |
| `WGR_MATERIAL_DEBUG_VIEW=base` (albedo only, `shade()` skipped) | **0.24** | 0.04 | the pass is per-fragment shading |
| `WGR_FOLIAGE_A2C=0` | 2.29 | 0.34 | alpha-to-coverage costs nothing (no `sample_mask` writes) |
| `WGR_GTAO=0` | 1.72 | 0.28 | −0.7, plus the 0.84 ms GTAO pass itself |
| `WGR_INTERIOR_SKY=0` | 1.84 | 0.26 | −0.56, plus its 0.7 ms depth maps |
| `WGR_SHADOW_MAPS=0` | 2.11 | 0.31 | −0.3 |
| `WGR_MAX_ACTIVE_LIGHTS=0` | 2.46 | 0.37 | 0 |

Reading: 0.11 M triangles cannot cost 2.4 ms as geometry; the cutout cards cover roughly 40%
of this frame's pixels (the pines on the right), and every one of those fragments runs the
full `shade()` — CSM, terrain and cloud shadow, sky irradiance, the two screen-space
occluders, fog, specular, the leaf-SSS term. No single term dominates; the two screen-space
occluder lookups are the largest, and they make the least sense on a leaf card.

## The lever

`shade()` gains a `screen_ao` bool. Vegetation cutouts pass false: no GTAO texture load, no
bent-normal load, no interior-sky volume walk. They keep their own canopy occlusion term
(`foliage_shadow_ao`) and every other term. Pipeline override `foliage_screen_ao` on the object
colour pipelines, `WGR_FOLIAGE_SCREEN_AO=1` restores the old path. The tree crops
(`.tmp-veg/ao_on_off_trees.png`) are indistinguishable.

## Measured after the change — and the honest caveat

Three alternating runs per arm, same session:

| arm | colour alpha-test (mean / min) | GPU frame |
|---|---:|---:|
| flag on (old behaviour) | 1.74 / 1.70 | 20.8 |
| flag off (default) | 1.74 / 1.71 | 22.6 |

The flag was below the spread — and the reason turned out to be a hole, not a mystery
(found 2026-09-03 00:05, fixed in the commit that adds the constants to `cull.rs`): the
GPU-driven object pipelines are built in `cull.rs` with their **own** pipeline-constant list,
and `foliage_screen_ao` was only in the per-draw list in `mod.rs`. On the path that draws
Everon's trees the shader default (0 = skip) applied whatever the env said, so "flag on" never
restored anything. The previous shader rebuilt from the parent commit read 2.37 / 2.41 / 2.37
(mean 2.38) — the same as "flag on" once the flag reaches the pipeline:

| arm (constant reaching both pipeline families) | colour alpha-test mean / min | GPU frame |
|---|---:|---:|
| `WGR_FOLIAGE_SCREEN_AO=1` (old behaviour) | 2.41 / 2.38 | 23.3 |
| default (vegetation skips GTAO + interior sky) | **1.66 / 1.50** | 20.9 |

**−0.75 ms, 31% of the pass, and the mechanism is exactly the one claimed.** The lesson is in
memory and in the `cull.rs` comment: an override must be in *every* pipeline family's
constants list, and an A/B that reads "no difference" is first a question about whether the
constant reached the pipeline.

## What follows on this row

The remaining cost is `shade()`'s per-fragment work on foliage: sky irradiance, fog, specular,
the SSS term. A distance-tiered foliage shading (full near, reduced beyond a card's on-screen
size) is the shape of the next lever; the census tool to guide it is a fragment count per
pass, which REN-GRS-001 built for grass and objects do not have yet.
