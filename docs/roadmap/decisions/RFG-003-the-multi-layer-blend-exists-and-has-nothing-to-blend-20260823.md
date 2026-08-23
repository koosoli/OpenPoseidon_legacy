# RFG-003 — The MatPBRMulti blend shader already exists. The exporter never gives it anything to blend.

**Date:** 2026-08-23
**Status:** root-caused and measured; the fix is an exporter change plus a re-export
**Follows:** [RFG-002](RFG-002-lod0-uses-a-shared-tile-while-the-object-bake-sits-beside-it-20260823.md)

## The shader is not missing

[HANDOVER-20260823b](../HANDOVER-20260823b-everon-renders.md) records "`MatPBRMulti` has no blend
shader". That is no longer true, and possibly never was: `gpu_driven.wgsl` carries a complete
per-fragment layer blend — one `MaskMap` sample, its RGB weighting three layer tiles under their own
`UVTransform_N`, the unpainted remainder falling to layer 0, renormalised where the mask over-paints,
and the same weights reused for the normal blend. `composeMultiLayers` defaults to **true**. Both the
second UV channel (`Vertex::uv1`, `MLODWriter::secondUVChannel`, `WGR_MODEL_SECTION_MASK_UV1`) and the
slots (`MaterialSlot::Mask`, `LayerColour1..3`) exist end to end.

It never runs, because `sm.mask_slot` is 0 for every material in the corpus.

## Measured, over the whole deployed material corpus

`PoseidonTools model emat <game>\reforger\everon\textures --keys`, 1,793 files, 100% parsed:

| key | count |
|---|---|
| `BCRMap` | **1,793** (100%) |
| `Enabled_2` / `Enabled_3` / `Enabled_4` | 910 / 892 / 820 |
| `Color_1..4` | 804 / 881 / 850 / 792 |
| `UVTransform_1..4` | 504 / 488 / 475 / 458 |
| **`MaskMap`** | **0** |
| **`BCR_1..4`** | **0** |
| **`NMO_1..4`** | **0** |

`MatPBRMulti` is 922 of the 1,793 — **51% of the corpus**. Every one of them carries the layer
*parameters* (which layers are on, their tints, their UV transforms) and **not one** carries the mask
or the tiles those parameters describe. For comparison, the same keys over Reforger's own 10,312
`.emat` files: `MaskMap` 4,044, `BCR_1` 3,905, `NMO_1` 3,768.

## Why

`XobCommand.cpp` collapses the material at export time. For a `MatPBRMulti` with no `BCRMap` of its
own it picks a layer tile, **rescales it so `Color_N` is its mean**, writes that single texture as
`BCRMap`, and drops the rest. The `MaskMap` is opened — but only to average its channels into scalar
layer weights for choosing the tint. Neither it nor the tiles are ever exported as textures.

The approximation was reasonable when the runtime could not blend. The runtime can, so the
approximation is now the only thing standing between the corpus and the real material — and it is
what the owner sees as a flat cream church at close range (RFG-002).

## What the fix is

1. **Export the textures.** `MaskMap` and `BCR_1..4` (and `NMO_1..4` for the normal blend) as `.paa`
   beside the material, instead of collapsing them.
2. **Write them into the `.emat`.** `MaskMap`, `BCR_1..4`, and the `UVTransform_N` already being
   written. `TranslateEmat` already routes these to `Mask` / `LayerColour1..3` / `LayerNormal1..3`.
3. **The layer count lines up exactly.** Enfusion's base layer 1 becomes `texture_slot`; layers 2, 3
   and 4 become `layer_slot[0..2]`, weighted by the mask's R, G and B, with the remainder falling to
   the base — which is verbatim what the shader already implements.
4. **`Color_N` still has to be applied per layer.** The shader blends untinted tiles today. Cheapest
   correct route: keep the exporter's existing rescale-to-`Color_N`-mean and apply it **per exported
   tile** rather than to one collapsed tile. No shader change, at the cost of one tile copy per
   object per layer.
5. **UV set 2.** These materials say `UVSrcGlobalMaps "UV set 2"`, so the mask is not in the same
   unwrap as the layers. `MLODWriter::secondUVChannel` and `WGR_MODEL_SECTION_MASK_UV1` are there for
   this and need to be fed.

Cost: exporter work, then a full re-export of the model tree (~1,248 models, ~45 min) and the
~6 GB robocopy into the game folder. Nothing lands for the owner until that re-export runs — see
CLAUDE.md rule 0b.

## What this also explains

The engine's per-material log line `Wgpu material: normal map NONE ... family=MatPBRMulti (no
NormalMap slot)` fires for every one of these. It is not a missing normal map in the source; it is
`NMO_1..4` never having been exported either.
