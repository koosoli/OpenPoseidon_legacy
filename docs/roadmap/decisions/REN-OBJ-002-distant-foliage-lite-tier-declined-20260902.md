# REN-OBJ-002 — A distant-foliage lite shading tier: measured, no gain, not shipped

**Date:** 2026-09-02 (night)
**Branch:** `asset-obs-001` (the change was built, measured and reverted; it is not in the tree)
**Follows:** REN-OBJ-001 (the alpha-tested object pass is per-fragment shading)

## What was tried

`shade()` gained a `lite` bool for vegetation cutouts past a camera distance (`WGR_FOLIAGE_LITE`,
default 150 m): no cascaded shadow-map lookup (terrain and cloud shadow kept), no specular lobe,
no local-light loop. Everything near, and everything not vegetation, unchanged.

## Measured (Everon `perf_combat`, three alternating runs per arm, same session)

| arm | `Objects: colour alpha-test` mean / min | GPU frame | CPU frame |
|---|---:|---:|---:|
| lite at 150 m (default) | 1.73 / 1.70 | 21.8 | 24.6 |
| `WGR_FOLIAGE_LITE=0` | 1.75 / 1.72 | 22.2 | 24.8 |

Below spread. Two reasons, both already in REN-OBJ-001's ablation table: the three terms the
tier removes are worth at most 0.3 ms together on this scene (shadow maps −0.3, lights 0,
specular not separately measurable), and on this pose most foliage fragments are *near* — the
pines that fill the right half of the frame stand inside 150 m — so the tier applies to few of
the fragments that cost.

## Decision

Reverted, not shipped: a tier with no measured gain is dead complexity in the one function
every fragment runs. The far-tree crop (`.tmp-veg/lite_off_on_far.png`) is indistinguishable,
which says the tier is *safe*, not that it is *worth it*.

## What this leaves for the row

The pass costs 1.74 ms as per-fragment shading whose remaining terms — sky irradiance,
fog, the SSS fill, the normal-map sample and decode, `mip_alpha_boost` — were not split
tonight. The honest next step is the tool, not another guess: a fragment census for the
object passes (REN-GRS-001's striped atomics, on the object fragment entries) so the pass
can be quoted in nanoseconds per fragment and compared with grass, whose colour pass shades
a similar screen area for 1.7 ms. Until then `WGR_MATERIAL_DISABLE_NORMAL=1` and
`WGR_MATERIAL_DEBUG_VIEW=lit` are the two remaining cheap splits.
