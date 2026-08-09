# MAT-030 — TreeAdv / TreeAdvTrunk / TreeAdvTrans stage semantics

**Date:** 2026-08-09
**Ticket:** MAT-030 (shader families)
**Status:** Mapping established from measurement; MCA channel packing deliberately unclaimed.

## Why this needed evidence

The Arma 3 vegetation path does not use `Super`. Every tree material in the shipped
game names `TreeAdv`, `TreeAdvTrunk` or `TreeAdvTrans`, and until now none of the
three had a schema — so the translator bound nothing for them and every tree drew
with no material contribution at all. Reaching for Super's table would have been a
guess: Super's Stage2 is Detail and its Stage3 is Macro, and neither is what these
families put there.

## Corpus

All 615 RVMATs in the four shipped vegetation PBOs, extracted locally and parsed:

| PBO | TreeAdv-family materials |
|---|---|
| `Addons/plants_f.pbo` | 100 |
| `Expansion/Addons/vegetation_f_exp.pbo` | 204 |
| `Enoch/Addons/vegetation_f_enoch.pbo` | 225 |
| `Argo/Addons/vegetation_f_argo.pbo` | 18 |
| **total** | **547** |

Family split: `TreeAdv` 369, `TreeAdvTrunk` 172, `TreeAdvTrans` 6. The remaining 68
materials in those PBOs are `Grass` (38), `Multi` (13), `Super` (9) and `Normal` (5).

## Measurement

Two independent witnesses agree, the same pair that settled Super: a procedural stage
names its own role in its tag (`#(argb,8,8,3)color(0.5,0.5,0.5,1,MCA)`), and a file
stage carries a suffix.

**TreeAdv** (n=369)

| stage | present | roles |
|---|---|---|
| Stage0 | 0/369 | absent in every one — the face texture is the colour |
| Stage1 | 369/369 | 301 `_nohq`, 62 procedural `nohq`, 2 `_no`, 1 procedural `no`, 3 `_ca` |
| Stage2 | 369/369 | 228 procedural `mca`, 129 `_mca`, 8 `_mc`, 3 procedural `mc`, 1 procedural `dt` |

Stage1 `uvSource` is `tex` in 369/369. Stage2 is `tex1` in 316 and `tex` in 53.
Exactly one material declares anything beyond Stage2 (Stage3..Stage7).

**TreeAdvTrunk** (n=172)

| stage | present | roles |
|---|---|---|
| Stage0 | 4/172 | 4 `_co` |
| Stage1 | 172/172 | 158 `_nohq`, 11 `_no`, 2 procedural `co`, 1 procedural `nohq` |
| Stage2 | 172/172 | 65 procedural `mca`, 64 `_mca`, 39 `_mc`, 4 procedural `mc` |

`VertexShaderID` is `TreeAdvTrunk` in 172/172. No material declares a stage past 2.

**TreeAdvTrans** (n=6) has TreeAdv's shape exactly: Stage1 normal 6/6, Stage2 MCA 6/6,
no Stage0.

## Decision

```
TreeAdv        Stage1 -> NormalMap   Stage2 -> MacroAmbient
TreeAdvTrunk   Stage0 -> BaseColour  Stage1 -> NormalMap   Stage2 -> MacroAmbient
TreeAdvTrans   Stage1 -> NormalMap   Stage2 -> MacroAmbient
```

TreeAdv's Stage0 is marked as a stage the family does not use, because it is absent
in all 369. TreeAdvTrunk's is mapped, because 4 materials do declare it. Everything
past Stage2 stays unmapped for all three: one witness does not describe a family.

### MCA gets its own slot

`MacroAmbient` is a new `MaterialSlot`, not an alias for Super's `Macro`. The families
tag the stage `MCA`, and they tag it that way in 357 of the 547 procedural stages
where the tag is legible — a name the corpus keeps distinct from `MC`. Declaring the
two the same texture semantic is a claim that needs its own evidence, and does not
have it. The `_mc`-suffixed minority (47 materials) still land in `MacroAmbient`,
because the slot the family assigns decides the role; their suffix is recorded as a
disagreement rather than used to reroute them, the same way Super's `env_land_co.tga`
is handled.

### MCA channel packing, measured

BI's tree documentation describes MCA as a macro in RGB with ambient occlusion in
alpha. Documentation is not a measurement, and the NOHQ/SMDI/AS maps here were
measured (n=40, n=40, n=25), so MCA was measured the same way: all 181 `_mca`
textures in the four vegetation PBOs, decoded and scanned per channel.

| | result |
|---|---|
| RGB | varies in 179/181; grey in 0/181; 40/181 chromatic by a channel-mean spread over 2 — a colour triple, not a replicated scalar |
| alpha, DXT1 files | 128/181, no alpha channel; decodes to a constant 255 |
| alpha, DXT5 files | 53/181, all carrying alpha: 47 varying over the full 0..255, 6 constant at 174 |

The split is exact — every constant-255 alpha belongs to a DXT1 file and every DXT5
file carries a real one. That is the documented behaviour showing up in the data: the
occlusion term is optional, and it is absent by being an absent alpha channel rather
than by a flag. Declaring the channel unconditionally is therefore safe, because the
DXT1 majority decode to 255, which is an occlusion term of "none".

`ChannelsForSlot(MacroAmbient)` sets `colour = R` (the RGB triple) and
`ambientShadow = A`. It deliberately does **not** set `alpha`: a consumer that read
this as transparency would punch holes through bark wherever the occlusion is dark.

## Runtime verification

Production draw path, WGPU, `WGR_GPU_DRIVEN=0`, no debug material selector, against
the shipped `a3\plants_f\tree\t_pinusp3s_f.p3d` (ODOL 73, 13 LODs, 5,931 faces at
visual LOD 0) placed by `--test-model` in a live mission world:

- section 0 → `t_pinusp3s_f_lod1_noatlas.rvmat`, `shader=TreeAdvTrunk schema=true`,
  `NormalMap` stage 1 resolved to `bark_pinussylvestris_l3_nohq.paa` (`exists=true`,
  `procedural=false`), `MacroAmbient` stage 2 resolved to
  `t_pinusp3s_f_lod1_noatlas_mca.paa` with `uv=tex1`;
- section 1 → `t_pinusp3s_f_lod1_polyplane.rvmat`, `shader=TreeAdv schema=true`, both
  stages procedural and therefore correctly not sampled;
- the same resolution repeats identically across all 13 LODs.

Toggling `WGR_MATERIAL_DISABLE_NORMAL` changes 35.2% of the trunk's pixels with a peak
per-pixel delta of 207, against a peak of 3 over an unlit sky control region — the bark
normal map is reaching the shader, not merely resolving. The canopy's peak delta is 6,
which is the expected non-response of a flat procedural normal.

A second model from a different DLC, `a3\vegetation_f_enoch\tree\t_betula_pendula_3s.p3d`
(ODOL 73, 13 LODs, 13,494 faces), resolves both families the same way across all 13
LODs, and unlike the pine its foliage section carries a *real* NOHQ file rather than a
procedural one — so the mapping is confirmed on independently authored content with a
non-procedural Stage1 on both sections.

At 16 m that model looked wrong — white branches and a canopy thinned almost to
nothing — and the first reading of it here was a base-texture failure. That reading was
mistaken and is recorded because the correction is the useful part. At 4 m the same
model through the same path draws correctly: green foliage, and white bark, which is
what *Betula pendula* bark actually is. Nothing was falling back to white.

What is left is a distance question, not a material one: at 16 m the canopy's leaf
cards are sparse enough to read as a skeleton against bright sky. That is either
correct LOD behaviour or an alpha-coverage problem — mip-averaging a 1-bit cutout
whose Stage0 is 87% fully transparent drives alpha below the test threshold, and
foliage thins with distance in exactly that way. It is unresolved, it is not caused by
this mapping (identical with `WGR_MATERIAL_DISABLE_NORMAL` set and unset), and it is a
plausible lead for the distance-dependent canopy artefacts reported earlier.

GL33 is not a control for this. Asked the same question from the same camera it draws
the birch as one opaque dark mass with no cutout applied at all, which is a worse answer
than the one being questioned.

Evidence kept out of the repository, per the local-asset rule:
`C:\Users\mail\AppData\Local\Temp\poseidon-treeadv-trace\` (`tree-normal-on.png`,
`tree-normal-off.png`, `birch-normal-on.png`, `birch-normal-off.png`, and the matching
`--test-model` logs).

## What this does not establish

- That anything *uses* MCA. The slot is bound and its channels are named; no renderer
  path samples it, so no macro tint or occlusion term reaches a pixel yet.
- That the canopy is lit correctly. Two-sided foliage lighting, `TreeAdvModNormals`
  vertex-normal modification, and the alpha/cutout path are separate questions this
  ticket does not touch; the previously reported dark patches on canopy tops are not
  explained by stage mapping and remain open.
- Anything about Arma 1/2 vegetation families, which were not in this corpus.
