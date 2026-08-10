# MAT-033 — binding the specular map the constant was written for

- **Ticket:** MAT-033
- **Date:** 2026-08-10
- **Replaces, in part:** the `WGR_MATERIAL_SPEC_UNMAPPED` approximation from `4b4bd5f`
- **Depends on:** MAT-031 (the families that name a specular map), MAT-032 (procedural constants)

## The problem the approximation stood in for

In Arma 3 a material's `specular` constant is the *maximum* a per-texel specular map
scales toward. Applying it with no map gives every texel that maximum, which lit
whole rooftops as mirrors. `4b4bd5f` scaled it to 0.25 — a stated approximation of a
real map's mean, tunable because the number was a look judgement rather than a
measurement.

Nothing could replace it until the map reached the shader, and the map could not
reach the shader because `WgrModelMaterial` carried exactly one texture. The
GPU-driven retained set owns essentially every Stratis section (6,066 against ~7 a
frame through `DrawSectionTL`), so a per-draw-only lookup reaches almost nothing.

## What now happens

`WgrModelMaterial` carries `specular_texture_id` across the C ABI (80 → 88 bytes;
both size assertions updated). `SectionMaterialGpu` gains `specular_slot` in what
was its tail padding, so the GPU-side layout is unchanged in size — and
`gpu_driven_shadow.wgsl` mirrors the rename, because it reads the same buffer and
every field after a mismatch would read the wrong bytes.

`ResolveMaterialSpecularMap` resolves the RVMAT's `SpecularDetail` slot the same way
`ResolveMaterialBaseColour` resolves `BaseColour`, off the same translation cache.

## Only the specular LEVEL is consumed

SMDI's G is the specular level and B is gloss — measured, not folklore: across 40
sampled SMDI textures R and A are constant 255 while G and B vary
(`MaterialChannels.hpp`). **G scales the constant. B is deliberately not used.**

Turning gloss into a Blinn-Phong exponent is a conversion with its own evidence
requirement — the same one that keeps `GlossToRoughnessUnvalidated` out of the
translation path and off by default. The map is bound and the channel is present for
whenever that question gets answered properly.

## Why the mask is a separate field

`shade()` srgb-decodes its material terms. Folding a linear per-texel mask into the
gamma-space `specular` colour before that decode would put the mask through the sRGB
curve: at SMDI's measured mean (G ≈ 47/255 = 0.184) that is 0.028 — a highlight
roughly a seventh of its intended strength. `ShadeMaterial` therefore carries
`spec_mask` and applies it *after* the decode. The per-draw path sets 1.0, leaving
its flat constant exactly as before.

## Result on Stratis

Whole-world object census, 436 materials reporting a `SpecularDetail` outcome:

| | materials |
|---|---:|
| specular map **bound** | **206** (151 file `_smdi`, 55 procedural constants) |
| no `SpecularDetail` slot | 230 |

By family, everything that names a specular map now binds one: `Super` 181,
`NormalMapSpecularDIMap` 16, `SuperAToC` 4, `NormalMapDetailSpecularMap` 3,
`NormalMapSpecularMap` 2. Those 206 now apply their **full** authored constant with
the map scaling it per texel; `MaterialSpecularScale` returns 1.0 for them, because
an approximation of a quantity applied on top of the quantity itself would darken it
twice.

The 55 procedural entries are why MAT-032 had to land first: `color(1,0,1,0,SMDI)`
is a *specular level of zero*, an authored matte surface. Without a generator those
materials would have fallen back to the 0.25 approximation and glinted where the
author said they should not.

## What the approximation is still for

One family, and now a named one: **`Multi`, 114 materials, 90 of them carrying a
bright constant (≥ 0.5) with no map this schema binds.** Multi's specular lives in
its `_dtsmdi` stages 5–8, a combined detail+specular texture with no slot in the
current set — splitting it is exactly the guess MAT-030 refused. `TreeAdv`,
`TreeAdvTrunk`, `Normal` and the rest report no map either, but their constants are
dark, so the approximation costs them nothing.

So `WGR_MATERIAL_SPEC_UNMAPPED` is no longer a global fudge over "everything with a
material"; it now covers one measured family with a known fix.

## What is NOT in this change

**Normal maps.** The GPU-driven path has no tangent frame — `VsOut` carries no
tangent, and deriving a per-pixel cotangent frame from screen-space derivatives is a
larger change with its own visual risk. Normal maps therefore still reach only the
~7 sections a frame that go through `DrawSectionTL`, which on an Arma 3 world is
effectively nothing. That is the next piece, not a claim made here.

## Verification, and its limit

- The frame renders with wgpu validation on and no shader or binding error, so the
  sample is legal and reached.
- The material-level counts above are measured over the whole Stratis object set.
- The OFP training capture is unchanged (`spec_mask` is 1.0 on that path).

**The look was not verified by image comparison, and cannot be with this harness.**
Two captures of the same camera, same build differ by mean 67/255 — the scene is not
frame-deterministic — so a pixel diff cannot attribute a change to specular. Whether
the result *looks* right on sunlit Arma 3 surfaces is a judgement for the owner.
