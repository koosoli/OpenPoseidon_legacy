# MAT-047 — A PAA with no FLAG tagg disables alpha cutout globally

**Date:** 2026-08-15
**Branch:** `codex/wgpu-performance-modernisation`
**Status:** PARTIAL — root cause fixed and pinned by tests; three related gaps remain open.
**Related:** [RND-032](RND-032-single-lod-xob-export-cost-6-5x-of-gpu-frame-20260815.md)

## The finding

`PAAEncoder::WritePAA` wrote what its own comment called a "minimal PAA — valid without them", omitting
the `TAGG` block. That is valid as a container and fatal as a material signal, because **the runtime
never inspects pixels to decide whether a texture is alpha-tested**:

- `PacPalette::Load` (`engine/Poseidon/Graphics/Rendering/Font/Pactext.cpp:173-185`) sets `_isAlpha`
  **only** from the `FLAG` tagg.
- `TextureSourcePac::IsAlpha()` returns it; `TextureWgpu::GetAlphaClass()` feeds it to
  `ClassifyTextureAlpha`, whose first line is `if (!hasAlpha) return Opaque` (`PAADecoder.cpp:57`).

So every converted `.paa` classified **Opaque**, received `alpha_ref = 0`, and drew its fully
transparent texels solid. Chain-link fences, railings, nets, window glass and tree polyplanes all
rendered as filled sheets of their own texture. The reported symptom was "building textures do not
work"; the textures were correct and the cutout was off.

## Denominator

Measured over the installed corpus, not sampled:

| | count | of total |
|---|---|---|
| textures carrying real alpha | 140 | of 714 (**19.6%**) |
| — all DXT5, 0% partial alpha (so all should route Cutout) | 140 | of 140 |
| models with >= 1 face using one | 424 | of 1132 (**37.5%**) |
| LOD0 faces affected | 373,332 | of 8,197,099 (4.6%) |

## Verification

Same camera, 3.8 m from `netfence_01_4m` at (7177.8, 2368.9):
`TEX_fence_3m.png` (solid white slabs) vs `TEX_fence_3m_AFTER.png` (see-through frame, the bin and
shed behind it visible). **53.3% of viewport pixels changed; mean luma 170.3 -> 129.1.** Also
`TEX_house_close.png` vs `TEX_house_close_AFTER.png`. All under `C:\Users\mail\AppData\Local\Temp\rf\cap\`.

## Ruled out with evidence, so nobody re-checks them

- PAA **pixel data** is fine — brick decode is pixel-perfect and `PoseidonTools` agrees with BI's
  `ImageToPAA` (second-witness method per `bi-paa-decoders-agree`).
- **LOD0 UV density** is fine.
- **Specular constant** — `WGR_MATERIAL_SPEC_UNMAPPED=0` changed nothing.
- **GPU-driven path** — `WGR_GPU_DRIVEN=0` changed nothing on the near fence.
- **Gamma / wrong-texture-bound** hypotheses: neither survives the fence A/B.

## The other half of the reported symptom is LOD, not texturing

At a 20 m in-town camera the histogram is `lod0=9 lod1=26 lod2=78 lod3=1206 lod4=7111`. Measured
area-weighted median texel density over all 702 structure models (1024 texture): **LOD0 815, LOD1 707,
LOD2 495, LOD3 354, LOD4 177 (p10 70), LOD5 86, LOD6 30 texel/m**. The smeared, contour-banded walls
are that, and RND-032 explains why LOD3/4 is selected at 20 m: `_lodInvWidth` is a framerate-adaptive
control over a 16x range, not a width-derived constant.

## Changes

- `engine/Poseidon/Graphics/Textures/PAAEncoder.cpp` — write the `FLAG` tagg with `PicFlagAlpha` when
  any texel has alpha < 255.
- `tests/unit/engine/Poseidon/Graphics/test_paa_encoder.cpp` — two byte-level tests pinning it; all 16
  `[PAAEncoder]` cases pass.
- `PoseidonTools` rebuilt. **`PoseidonGame` needed no change** — this is entirely converter-side.

**The installed tree was modified**: 140 `.paa` under
`D:\SteamLibrary\...\reforger\everon\textures` had the 16-byte tagg inserted losslessly (no re-encode)
so the fix was visible without re-exporting the corpus. Originals are backed up at
`C:\Users\mail\AppData\Local\Temp\rf\tex-noflag-backup`. Re-running the converter with the fixed
encoder produces identical files.

## Still open

1. **`GlobalMapsUVTransform` is ignored by `XobCommand`.** `NetFence_Wire.emat` declares
   `TilingU 24 TilingV 24`; without it the net cutout is magnified 24x and resolves as thick ribbons
   rather than chain-link. **1,208 of 10,312 `.emat` declare it.**
2. **`TextureBaker::Write` (`XobCommand.cpp` ~758) overwrites the albedo's own alpha with 255**
   whenever the material names no `OpacityMap`. Only **24 of 309** baked `_MLOD` atlases carry alpha,
   which is why distant fences remain solid cards even with the FLAG fix.
3. Nearly every `.emat` resolves **no BaseColour, no normal map and no specular map** — the face
   texture is carrying everything.
