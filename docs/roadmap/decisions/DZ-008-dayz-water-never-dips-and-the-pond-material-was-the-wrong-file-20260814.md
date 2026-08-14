# DZ-008 — DayZ water never dips the terrain, and the pond material we were reading was the wrong file

**Date:** 2026-08-14
**Branch:** `dayz-compat-layer`
**Supersedes premises in:** [DZ-006](DZ-006-a-constant-colour-is-a-real-albedo-and-never-reflect-onto-an-unbound-one-20260814.md)
and the "terrain failing to dip" hypothesis in
[HANDOVER-20260814](../HANDOVER-20260814-dayz-water-and-terrain.md).

## The question this answers

The handover asked, of Chernarus's white-blob ponds: *is the terrain under those ponds failing to
dip, or are the quads at the wrong Y?* It named the missing depression as "a far more fundamental
bug than anything water-related", and proposed restoring the original engine's per-body
`Landscape::_waters` levels as the eventual fix.

**Measured answer: neither, and the depression is not missing — DayZ never authors one.**

## 1. No DayZ water placement sits over a cell the world marks as wet

New tool: `PoseidonTools terrain probe <wrp> [--at X Z] [--model SUBSTR]`. It bilinearly samples the
OPRW elevation grid under every placement whose model path matches a substring, and reports the
distribution of `placementY − terrainY` together with the `Oprw25Geography` water-depth bits of the
land cell underneath. One pond says nothing about the other 669, so this reports populations.

| population | n | median Δ | mean Δ | p05..p95 | cells with `minWaterDepth > 0` |
|---|---|---|---|---|---|
| ChernarusPlus `ponds\` | 670 | +0.230 m | +0.144 m | −3.27 .. +2.67 | **0 / 670** |
| ChernarusPlus `stream` | 3,374 | +0.058 m | +0.005 m | −0.84 .. +0.72 | **0 / 3,374** |
| Enoch `ponds\` | 495 | +0.400 m | +0.488 m | −4.41 .. +6.38 | **0 / 495** |

**0 of 4,539 water placements** sit over a cell carrying any authored water depth, and the placement
Y is centred on the terrain surface — streams to within a **5 mm** mean. `maxWaterDepth` is non-zero
under just 17 of the 670 Chernarus ponds and 0 of Enoch's 495.

The named pond cluster probes exactly as predicted by that: at `X=3965 Z=2330` the terrain is
**2.656 m** and the placement Y is **≈2.7** — a delta of about **4 cm**.

### What follows

* **Coplanarity is the designed condition, not a defect.** DayZ water is a ground-hugging surface
  mesh, not a filled depression. The renderer has to handle a sheet lying on the terrain — depth
  bias, or a decal-style ordering — and expecting a hollow will never be satisfied.
* **`Landscape::WaterDepth(x, z)` returning 0 is *correct* for DayZ**, not a stub bug. The water
  renderer's comment that "water existence is defined by the terrain dipping below sea level" is
  structurally inapplicable to this generation.
* **Restoring `_waters` does not fix the ponds.** A per-body water level z-fights identically
  against coplanar terrain. It remains the right destination for other reasons (DZ-003), but it is
  not this defect's fix, and the handover's framing of it as such should not be carried forward.
* Ponds spread ±9 m while streams spread only ±2 m, because a pond mesh has banks — its placement
  origin is not its water plane. Do not read the pond Δ as a water-surface offset.

## 2. The pond material is `pond_moss_1/2.rvmat`, not `water_lake.rvmat`

Scanning the 1,244 water P3Ds for referenced `.rvmat` paths:

| corpus | n | with rvmat | materials |
|---|---|---|---|
| Chernarus `ponds\` | 182 | 182 | `pond_moss_1.rvmat` 106, `pond_moss_2.rvmat` 75, `water_lake.rvmat` **1** |
| Chernarus `streams\` | 20 | 20 | `water_stream.rvmat` 13, `water_river.rvmat` 7 |
| Enoch `water_bliss` | 989 | 989 | `enoch_river.rvmat` 804, `enoch_pond.rvmat` 183, +2 |

**181 of 182 Chernarus ponds use a material no previous investigation opened.** All three files are
byte-for-byte identical 79-byte `CalmWater` stubs, so every conclusion drawn about `water_lake.rvmat`
still holds — but anyone grepping for `water_lake` to find the pond material finds 1 model in 182,
and would reasonably conclude the ponds were not reaching the water path at all.

## 3. `water_ext.paa` is on the Roadway LOD — it is not the drawn section's texture

DZ-006's rule (a procedural constant may override a face texture when the family is `CalmWater`) is
justified in-code by the claim that *"Every DayZ water p3d names [`water_ext.paa`]"* as its face
texture, and that this is a surface-type marker rather than an albedo. **The conclusion is right and
the premise is wrong**, which matters because the comment tells the next reader to look at the wrong
texture.

Classifying each LOD by `LodPurpose` (`3e15 = 1e15 * 3 = Roadway`, per `World/Model/LodPurpose.hpp`):

* `water_ext.paa` appears **only on the Roadway LOD**. That is a stronger version of DZ-006's own
  argument — a roadway LOD is *definitionally* the walkable-surface marker.
* Across the 1,244 water models, textures named on **visual** LODs (resolution < 1e5) are:
  * **1,025 `<default>`** — no texture at all; appearance is wholly the material's.
  * **182 `dz\data\data\normalmap1_dxt5.paa`** — every one of them a Chernarus pond.
  * the rest streambed stone, `streams_flowmap_ca.paa` (28 — the DZ-002 flow map), foliage.

`normalmap1_dxt5.paa` is a 1024×1024 **DXT5 normal map**: R=G=B mean **126.6**, alpha mean 126.8 with
**100 %** of texels partial (0 < a < 255), classified BLEND. So the population that renders as white
blobs — Chernarus's 182 ponds — is exactly and only the population whose visual section names a
**normal map in the stage-0/diffuse slot**, at a mean of ~0.50 albedo if sampled as colour.

That is a correlation over the full corpus, not a spot check, and it is the strongest lead the defect
has had. **It is not yet a proven cause**: `DrawSectionTL`'s DZ-006 branch is supposed to override a
`CalmWater` section's face texture with the near-black constant, which would discard the normal map.
Whether it actually does for these sections is the next measurement, and it needs a frame capture.

## Not measured, deliberately

A capture was **not** run. `EngineWgpu.cpp`, `shader3d.wgsl`, `gpu_driven.wgsl`, `terrain.wgsl`,
`ffi.rs` and `lib.rs` currently carry another agent's uncommitted in-flight work, so a build measures
that tree rather than HEAD, and any appearance judgement from it would be unattributable. Run the
capture once the tree is clean.

## Lead, not a claim

`dz\water\ponds\data\pond_moss_ca.paa` (and its `.edds` twin) sits beside the pond materials, matches
the 181 `*_moss.p3d` models by name, and is referenced by **no** model texture list and **no**
material. Something maps it by convention. Worth opening before assuming the ponds have no authored
colour.

## Tooling

`PoseidonTools terrain probe` — added in `apps/tools/Tools/commands/TerrainCommand.cpp`. Revision
18/20/24/25 worlds only. `--at X Z` dumps one point (bilinear height, the four corner samples, and
the decoded geography bits); `--model SUBSTR` reports the delta distribution, a histogram, and the
water-depth tallies over every matching placement.
