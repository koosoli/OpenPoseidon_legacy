# GRS — local Arma 3 clutter-tuft compatibility probe

**Date:** 2026-08-10
**Related:** `GRS-GATE-1`, `ASSET-010`, `engine/WgpuRenderer/docs/grass-plan.md`
**Status:** Local compatibility proven; not an approved or redistributable grass package.

## Scope and provenance

The owner-provided local Arma 3 install is an approved engineering reference under
`AST-006`. This probe used only:

- source PBO: `D:\SteamLibrary\steamapps\common\Arma 3\Addons\plants_f.pbo`;
- entry: `clutter\data\c_grassgreen_grouphard_ca.paa`;
- local converted output: CWA install `tmp\a3-c_grassgreen-grouphard.png`.

No source or converted Arma 3 file is added to this repository or an asset package.
The PAA is proprietary game content, so the roadmap's `ASSET-010` source-registry
route cannot treat it as a shippable replacement.

## Result

`PoseidonTools image inspect` reports a 1024x1024 DXT5 PAA with 73.2% clear,
12.2% opaque and 14.6% partial-alpha texels. After local conversion to PNG, the
existing non-invasive seam loaded it with:

```text
WGR_GRASS_TUFT=tmp\a3-c_grassgreen-grouphard.png
```

The installed WGPU smoke completed successfully and logged:

```text
Wgpu grass: mid-LOD clump texture uploaded (1024x1024, 4 src channels)
Wgpu grass: near-LOD photo blade atlas uploaded (8 layers, 64x256)
```

This proves the existing grass system can retain its compute placement, wind,
tracks, helicopter downwash, near-blade array, fallback, and terrain eligibility
while using a locally supplied Arma 3 alpha-tuft texture for the optional mid-LOD
card route.

## Boundary

This particular image is a multi-clump clutter atlas, not an isolated blade or a
quality-approved replacement for the current clump. It is useful for local visual
comparison only. A shipped grass package must follow the already-recorded order:
licence-compatible source -> `ASSET-010` registry -> deterministic processing ->
validated optional package -> visual/performance gate. The procedural fallback
remains the default until that gate passes.
