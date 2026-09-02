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

The flag is below the spread. **The owed reference was then run the same session: the
previous shader, rebuilt from the parent commit and deployed, three runs — 2.37 / 2.41 / 2.37
ms (mean 2.38).** So the change saves 0.64 ms (27% of the pass) on both flag values: what the
pass lost is the restructure (the two occluder lookups under one branch), not the skipping the
flag controls, and I cannot say from the source why the compiled shader is cheaper for the
non-vegetation fragments too. The number is measured either way; the mechanism is not
understood, and the record says so.

## What follows on this row

The remaining cost is `shade()`'s per-fragment work on foliage: sky irradiance, fog, specular,
the SSS term. A distance-tiered foliage shading (full near, reduced beyond a card's on-screen
size) is the shape of the next lever; the census tool to guide it is a fragment count per
pass, which REN-GRS-001 built for grass and objects do not have yet.
