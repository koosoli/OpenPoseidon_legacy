# WLD-017 — A terrain material is not a ground layer

Date: 2026-08-12
Branch: `grass-lod-dissolve-photo-cards`
Witness: official OA Takistan (OPRW 24) and Arma 3 Stratis (OPRW 25)

## The symptom

Takistan's terrain rendered as one flat sand colour, in a world that declares
3,293 surface materials and uses 3,292 of them. The renderer said so at info
level and nobody had read it:

```
Wgpu terrain materials: 2999 of 3293 authored LCA blends bound; 0 complete,
3033 texture bindings dropped at 1024-layer limit
```

## The cause

Two spaces had been conflated. Terrain **materials** live in a storage buffer
with no cap — the shader reads `terrain_materials[entry & CELL_LAYER_MASK]`, and
that mask allows 32,767. Ground **layers** live in a `binding_array` of at most
`WGR_TERRAIN_MAX_GROUND_LAYERS` (1,024) views, and a material's `legacy` field is
the index into it.

`UploadGroundTextures` bound one layer per material, so Takistan asked for 3,293
and every authored satellite/mask/base binding after the 1,024th was dropped.
`UploadIndexMap` then clamped each cell's **material** index to the **layer** cap,
collapsing 2,269 of Takistan's 3,293 materials onto material 1,023 — one texture
for two thirds of the map.

Neither defect is specific to Arma 2. Both are generic engine bugs that Arma 3
never triggered, because Stratis declares 479 materials and Altis is not far
past that. Takistan is the witness, not the subject.

## The fix

Deduplicate the ground-layer array by texture and let each material record the
layer it landed on. Takistan's 3,293 materials are drawn from **499** distinct
images; Stratis's 479 from **76**. Layer 0 is reserved for the white fallback so
a material with no cell texture costs no layer.

Then clamp the index map to the material count (bounded by `CELL_LAYER_MASK`'s
15 bits) rather than to the layer cap, since a cell names a material.

```
Takistan  2999 of 3293 bound, 2999 complete, 499 of 1024 layers,   0 dropped
          (was 2999 bound,       0 complete,                    3033 dropped)
Stratis    419 of  479 bound,  419 complete,  76 of 1024 layers,   0 dropped
```

## The metric that was lying

`complete` counted materials whose mask and base layer indices differed from the
satellite's. That only worked because duplicates were never merged. Stratis's
`gdt_strbeach_co.paa` is simultaneously a cell texture and an authored base
surface, so once deduplicated it correctly shares one layer — and the old test
read that as a failure, reporting 0 complete for a frame that was pixel-identical.

It now asks whether each authored image actually reached a layer, reported by the
binder rather than inferred from the index. Stratis reads 419 complete where the
old metric said 413: six materials had been miscounted in the other direction.

## Regression evidence

- Arma 3 Stratis airbase, same camera, before and after: mean pixel difference
  0.90/255 over a scene whose own run-to-run noise on this mission is ~0.05, all
  of it in animated water and cloud; terrain, structures and vegetation are
  visually identical. Census unchanged at 350 of 350 models.
- OFP Preview-0 WGPU smoke passed.
- Takistan day capture shows distinct cultivated, stony and sand surfaces where
  it previously showed one colour.

## What is still not right

The authored LCA composition is bound but not yet blended: the shader gates it
behind `a3_surface_blend`, which is off by default and capped at 0.35 when on.
So Takistan's terrain now takes the *correct* per-cell texture, but not the
mask-driven detail composition the RVMATs describe. That is the next piece of
work, and it is shared between the generations rather than A2-specific.
