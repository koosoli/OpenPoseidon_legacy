# MAT-039 — The `Multi` family is a four-layer masked blend, and we bind one layer of it

**Date:** 2026-08-12
**Related:** MAT-030 (family census), MAT-038 (retained-path normal maps)
**Status:** Cause established from the source material. Not implemented.

## The report

After the retained path gained normal maps (MAT-038) and the tangent frame was
made scale-invariant, one class of Takistan surface still looked flat: the brick
houses. Stone walls beside them looked correct. Reported live from free-fly
`(5760.49, 5536.90, 312.54) az 67.2 el -1.8`, where `house_k_5_ep1` stands ~9 m
away and `house_k_8_ep1` ~21 m ahead.

Every material on those houses **does** bind a normal map — traced. So this is
not a missing binding.

## What the source actually says

`ca\structures_e\housek\data\house_k_5_multi.rvmat`, `PixelShaderID Multi`:

| stage | texture | texGen |
|---|---|---|
| 0 | `rock\wall_01_co` | 0 |
| 1 | `loam\wall_brick01_co` | 1 |
| 2 | `loam\wall_dirt01_co` | 2 |
| 3 | `loam\ground_dirt01_co` | 3 |
| 4 | `house_k_5_mask` | 4 |
| 5–8 | procedural `DTSMDI` | 5,6,7,3 |
| 9 | `house_k_5_mc` (macro colour) | 4 |
| 10 | `house_k_5_ads` (ambient/detail shadow) | 4 |
| 11 | `rock\wall_01_nohq` | 0 |
| 12 | `loam\wall_brick01_nohq` | 1 |
| 13 | procedural `NOHQ` (flat) | 2 |
| 14 | `loam\ground_dirt01_nohq` | 3 |

Four surface layers, each with its own colour, its own normal, its own specular
and — critically — **its own TexGen**. Stage4's mask chooses between them per
texel. The fine brick relief lives entirely in layer 1, at layer 1's UV scale.

`ShaderSchema::kMulti` already records this honestly: Stage0 is
`BaseColour` commented *"layer 0 colour, standing in for the base"*, stages 1–3
are `MaterialSlot::Count` (no slot at all), and Stage4 is `Mask`,
*"preserved unused"*.

So the renderer draws layer 0 — the rock — with layer 0's normal at layer 0's UV
scale, and the brick simply never appears. The building is not missing its
normals; it is missing three of its four surfaces.

This is a **shared Real Virtuality semantic**, not an A2/OA one. `Multi` is used
by Arma 3 as well; Takistan just happens to put it on the buildings you stand
next to.

## What implementing it needs

Per section: 4 albedos + 4 normals + 1 mask + 4 UV transforms, against the
retained material's current 3 texture slots (albedo, specular, normal). That is
a real feature, not a patch:

- `WgrModelMaterial` grows to carry the layer arrays and their TexGens, or gains
  an indirection into a per-material layer table;
- `ShaderSchema::kMulti` gains slots for stages 1–3 and 12–14;
- `gpu_driven.wgsl` composes the four layers by mask before shading, and blends
  four tangent-space normals rather than sampling one.

The mask sampling and the four-way blend are the same shape as the terrain's
LCA composition in `terrain.wgsl`, which is worth reusing rather than writing
twice.

## Note on parallax / relief mapping

Worth stating so it is not attempted in the wrong order: parallax occlusion
mapping would be built on the *same* per-layer height/normal data this record is
about, and applying it to a `Multi` surface that is still drawing one of its four
layers would add apparent depth to the wrong texture. Fix the composition first;
POM is then a shading change on top of a correct surface, and can be judged on
its own merits.
