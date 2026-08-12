# MAT-038 — The retained path had no normal maps, on any generation

**Date:** 2026-08-12
**Related:** MAT-031 (normal-map cluster), MAT-033 (SMDI in the GPU-driven path),
MAT-037, WLD-018

## The asymmetry

`WgrDraw3D` — the per-draw path — has carried `normal_texture_id` since the
normal-map work landed. `WgrModelMaterial` — the retained, GPU-driven path — had
`texture_id` and `specular_texture_id` and **no normal slot at all**.

GPU-driven is on by default and the retained set owns essentially every world
section. MAT-033 recorded the same asymmetry for the specular map and measured
it: 6,066 sections reach the adapter with a material attached, against 7 sections
a frame through `DrawSectionTL`. So in practice almost nothing in a rendered
world was normal-mapped — not on Takistan, and not on Stratis either.

The Arma 2 corpus is what made it visible: its surfaces are rougher and its
buildings are mud-brick and plaster, so flat shading reads as obviously wrong.
Stratis was equally flat and nobody had cause to notice.

## The change

`normal_slot` is added the way `specular_slot` was, along the same five hops:
`WgrModelMaterial` → `ffi.rs` → `SectionMaterialGpu` → `gpu_driven.wgsl`
(and the shadow shader's layout mirror) → sampled in the fragment shader.

The struct grows past its tail padding for the first time, so the padding is now
explicit on both sides rather than left to each language's rules, and the ABI
size assertions move 88 → 96. They are the reason this was caught at compile time
rather than as garbled shading.

`ResolveMaterialNormalMap` mirrors `ResolveMaterialSpecularMap` exactly, including
generating procedural Stage1 constants — `#(argb,8,8,3)color(0.5,0.5,1,1,NOHQ)`
is a genuine flat normal and binds like any other.

Shading reuses the per-draw path's approach verbatim: NOHQ decode (X from A, Y
from G, Z reconstructed) and a tangent frame recovered from the interpolated
UV0/world-position derivatives, orthonormalised against the geometric normal with
handedness taken from the original bitangent. That is what makes this need **no
vertex-format change** — the retained mesh carries no authored tangent stream and
does not need one.

## Coverage

Materials reaching the retained path, by outcome:

| world | normal map bound | none |
|---|---:|---:|
| Takistan (OA) | **358** | 4 |
| Stratis (A3) | **299** | 35 |

Every "none" is `no NormalMap slot` — a material that genuinely declares none.
There are no upload failures and no procedural-lookup misses.

## Regression evidence

- Takistan close-up: walls and plaster gain their authored relief; mean 0.47/255
  against the previous commit, concentrated on the surfaces that gained a map.
- Takistan dawn, the case that exposes normal-map errors: lighting darkens
  naturally, no inverted or blown-out facets, relief still legible.
- Stratis airbase: 1.15/255, renders correctly, no artefacts.
- OFP Preview-0 WGPU smoke passed. OFP surfaces come from CfgMaterials and have
  no RVMAT, so `ResolveMaterialNormalMap` returns 0 and the path is untouched.
- `[odol]` 22 passed / 2 skipped; `[Formats]` 23 passed / 1 skipped;
  `[graphics]` 10,534 of 10,536 — the same two pre-existing, unrelated failures
  as before the change (a hidden `[.fixture]` DDS helper and a stale
  `sizeof(SVertex)==36` assertion against a 60-byte struct).

## Not covered

Terrain normals. A3 authors a per-tile terrain normal at Stage14
(`n_<x>_<y>_no.paa`) and A2 authors per-surface `_nopx` maps at the odd stages of
its terrain materials; neither reaches `A3TerrainMaterial` or the terrain shader.
That is a separate slot on a separate path and is the remaining half of the
normals backlog.
