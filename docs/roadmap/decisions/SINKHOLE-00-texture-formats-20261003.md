# SINKHOLE-00 — W0: native PNG textures and the plain-DDS layouts (from Malprave)

Branch: `dec/sinkhole-w0-texture-formats`, on top of `dec/tidewater-w12e-docs-showcase` (95a9eb1d).
Ledger: `TEX-FMT-001`. Status: integrated, unit-tested; not yet seen in game.

## Why this is W0 of Sinkhole

Sinkhole is the workstream that brings The Forest's island into OP: terrain holes and caves
(W1/W2), swimming and diving (W3/W4), the island itself (W5). The island's textures are Unity
DXT1/DXT5 (most of them crunch-compressed). The plan is to decrunch them offline to the exact
DXT blocks and mips Unity hands the GPU, write them as `.dds`, and let OP's wgpu path upload the
blocks unchanged (`BcFormatFor`, `TextureWgpu.cpp`) — 4–8x less VRAM and bandwidth than a decoded
RGBA texture, which is the frame-rate case for 47k placed objects. The owner also asked for
native PNG, and for Malprave's DDS work (Dec's fork) to be brought over where OP lacks it.

## What changed

**PNG (and TGA/BMP/JPG) — `JpgImport.hpp/.cpp`.** `.png` was already routed to the stb source
(`SelectTextureSourceFactory`), but that source reported `PacARGB1555`, `IsAlpha() == false`,
and wrote A=255. On GL33 a PNG loaded with its alpha thrown away. On wgpu 1555 is neither a
block format nor 8888, so the upload fell through to the re-read-as-PAA fallback
(`DecodePAABuffer`), which cannot parse a PNG: **the texture loaded as nothing**. Now:

* the source reports `PacARGB8888` — the format both renderers already upload from a decoded
  source (wgpu: RFG-018 path, RGBA8 + GPU mips; GL33: `InitGLPixelFormat`);
* the decoded alpha is classified once at load, Malprave's rule: any value strictly between 0
  and 255 → `IsAlpha` (the wgpu `ClassifyAlpha` scan then tells cut-out from blend from the
  source pixels, `TextureWgpu.cpp` ~1313); only 0/255 with some 0 → `IsTransparent`
  (punch-through); all 255 → opaque;
* `ForceAlpha()` stays a no-op: `TextureWgpu::Init` forces alpha on every 8888 format, and an
  opaque PNG must not become blended on the format's say-so;
* the 1555 destination keeps the alpha bit from the pixel; 4444 added for completeness.

**Plain DDS — `DDSConverter.cpp`, `DdsImport.cpp`** (ported from Malprave `ddsTextures.patch`):

* uncompressed layouts are read **through their bit masks** into R,G,B,A bytes: 8/16/24/32-bit
  RGB(A), luminance (L8, A8L8), alpha-only (A8). The old reader took 32-bit only and assumed the
  bytes were already R,G,B,A — an A8R8G8B8 file, which is what most tools write, came out with
  red and blue swapped. The existing RGBA-masked files (OP's own `WriteDDS`) read identically;
* `DXT2`/`DXT4` FourCCs load as DXT3/DXT5 (same block layout);
* a DXT1 whose blocks use the 3-colour mode's index 3 is reported `IsTransparent`, so cut-out
  DXT1 foliage is alpha-tested rather than drawn as opaque black-edged quads.

## Not ported, and why

* Malprave's GL33 `DstFormat` 8888 fallback and its loose-texture resolver changes: GL33 here
  already maps 8888, and OP's wgpu path never calls the loose resolver (direct `.png`
  references do not need it).
* Malprave's "alpha from pixels for DXT3/5": OP's renderer already classifies those from the
  blocks (`ScanTopMipAlphaBlocks`), which is the same answer.
* Malprave's DXT1 "decode when mips are missing": OP generates missing mips itself.
* The `.paa`-only `IsAlphaOrdered` extension checks (`ShapeSetup.cpp`, `ShapeAdapter.cpp`) were
  reviewed and left alone: a texture with real alpha is already alpha-ordered by `IsAlpha()`;
  the extension rule only adds ordering to *opaque* PAAs (an original-engine quirk), and an
  opaque PNG staying unordered is correct.

## Verification

* New `tests/unit/.../Textures/test_sinkhole_texture_formats.cpp`: 8 cases / 45 assertions —
  PNG blend, cut-out and opaque classification, decoded alpha bytes, dispatcher; DDS A8R8G8B8,
  R8G8B8, R5G6B5, L8, A8L8, A8, DXT2/DXT4, DXT1 punch-through and the opaque control.
  Fixtures `tests/fixtures/png/*.png` are generated synthetic 32x32 images.
* `[sinkhole],[JPG],[DDS],[edds],[PNG]`: 68 cases pass. `[Graphics],[graphics]`: 478 pass, one
  failure — `I-05 FontSystem shutdown clears FreeType caches` (`test_shutdown_order_audit.cpp:187`)
  — a source audit of `UnloadGameData` that this branch does not touch.
* Evidence: `docs/roadmap/evidence/sinkhole-w0-20261003.json`.

## Not done / not verified

* **Not seen in game.** No harness capture yet of a PNG or masked DDS texture on a model or
  terrain. That run is part of W5 (the island ships its textures as `.dds`) and needs the `.tw`
  watcher running.
* No optional BC compression of large opaque PNGs at load (they upload as RGBA8; fine for
  authoring, not what shipped content should use).
* No DX10-header DDS (BC4/5/7 in plain `.dds`); OP reads those only from `.edds` today.
* `validate_compat_c1_ledger.py` already failed before this branch (AST-011B evidence hash);
  not touched here.
