# Preview 0 — capability matrix

**Generated file — do not edit by hand.**

Produced by `scripts/generate_capability_matrix.py` from
`docs/roadmap/renderer-systems-ledger.yaml` and `docs/roadmap/status-ledger.yaml`.
CI re-generates it and fails on drift, so this table cannot quietly disagree with
the ledgers it summarises. Regenerate with:

```sh
python scripts/generate_capability_matrix.py
```

| Rating | Meaning |
| --- | --- |
| **Works** | Present and complete against its own plan, on by default. |
| **Partial** | Present, with a stated gap. The gap is named in the row. |
| **Experimental** | Complete but off by default. You must opt in. |
| **Unavailable** | No implementation. Listed so absence is explicit rather than assumed. |

`Audited` is the date the entry was last checked item by item against the branch.
An older date means the claim is carried from a report, not re-verified — read those
rows as "exists, extent unconfirmed".

## Renderer systems

| System | Rating | Summary | Audited | Plan |
| --- | --- | --- | --- | --- |
| Cascaded shadow maps | **Partial** | Tier 1 is two of four. | 2026-08-03 | [plan](../../../engine/WgpuRenderer/docs/cascaded-shadow-map-plan.md) |
| GPU CDLOD water surface geometry | **Works** | Node selection and range computation are C++ side (BuildQuadtree, ComputeCdlodRanges); the instanced grid mesh and vertex morph are in water/mod.rs and water.wgsl, with GRID_N mirrored between them. | 2026-08-03 | [plan](../../../engine/WgpuRenderer/docs/water-cdlod-geometry-plan.md) |
| Water surface rendering | **Partial** | Plan is REVISED with stage 1 landed, and was the most recently maintained of the renderer plans. | 2026-08-06 | [plan](../../../engine/WgpuRenderer/docs/water-rendering-plan.md) |
| Consolidated WgrRenderParams FFI block | **Works** | Scope was FFI ergonomics only, with no shader, UBO or visual change, and it is complete against that scope. | 2026-08-03 | [plan](../../../engine/WgpuRenderer/docs/render-params-consolidation-plan.md) |
| Terrain sky-visibility ambient occlusion | **Works** | Heightfield sky-view factor feeding terrain ambient, plumbed end to end (WgrSkyVisibility, terrain_set_sky_visibility) with strength, floor, contrast and a debug lane exposed for tuning. | 2026-08-03 | [plan](../../../engine/WgpuRenderer/docs/sky-visibility-ambient-plan.md) |
| HDR pipeline — linear target, exposure, bloom, tonemap | **Partial** | Four of the plan's five stages, so PARTIAL rather than the SHIPPED this entry first carried. | 2026-08-03 | [plan](../../../engine/WgpuRenderer/docs/hdr-pipeline-plan.md) |
| Bindless object textures and samplers | **Works** | The plan's "uncommitted, pending user build" caveat is obsolete; committed 2026-07-09. SHIPPED is right for the scope the plan claims — it lists its own exclusions rather than leaving them implied, and they still hold: the shadow and 2D... | 2026-08-03 | [plan](../../../engine/WgpuRenderer/docs/bindless-textures-plan.md) |
| Opaque depth and normal prepass, Hi-Z | **Partial** | Stage-by-stage against the branch, which does NOT match the plan's status line ("Stages 0/2/3 still planned"): Stage 0 — NOT done. | 2026-08-03 | [plan](../../../engine/WgpuRenderer/docs/depth-prepass-plan.md) |
| GPU-driven culling | **Partial** | "Stages 1-3 live" (the RND-030 summary) overstates stage 3. The plan's own status block is the accurate one: Stage 1 retained data model — built, folded into CullState rather than as the doc's CPU-draw intermediary. | 2026-08-03 | [plan](../../../engine/WgpuRenderer/docs/gpu-culling-and-depth-plan.md) |
| Procedural sky and atmosphere | **Partial** | Considerably further than "stages 0 and 1 landed" (the RND-030 summary) and further than the plan's own "Stage 2 in progress". | 2026-08-14 | [plan](../../../engine/WgpuRenderer/docs/procedural-sky-plan.md) |
| Foliage translucency | **Works** | All three stages are implemented and live on the default path. | 2026-08-03 | [plan](../../../engine/WgpuRenderer/docs/foliage-translucency-plan.md) |
| Compute skinning bake | **Experimental** | Accurate as recorded. skin_bake_enabled defaults false; WGR_SKIN_BAKE=1 enables. | 2026-08-03 | [plan](../../../engine/WgpuRenderer/docs/compute-skin-bake-plan.md) |
| Screen-space ambient occlusion (GTAO) | **Partial** | Cost at Tier 1 resolution is ~32% of the GPU frame, against a headline figure of 8% measured in an 800x600 window. | 2026-08-05 | [plan](../../../engine/WgpuRenderer/docs/screen-space-ao-plan.md) |
| Geometry-aware interior sky visibility | **Partial** | Section 3d absent (disk cache, background scheduling, model-variance policy). | 2026-08-05 | [plan](../../../engine/WgpuRenderer/docs/interior-sky-visibility-plan.md) |
| GPU-placed three-ring grass field | **Partial** | GRS-GATE-1 unpassed: assets/ is gitignored, so the authored texture path is dead on any fresh clone and every user gets the procedural look. | 2026-08-05 | [plan](../../../engine/WgpuRenderer/docs/grass-plan.md) |
| GPU heightfield terrain rendering | **Partial** | HISTORICAL PRE-PERF-004 FINDINGS, BOTH SUPERSEDED. | 2026-08-14 | [plan](../../../# NONE — no plan document for terrain rendering exists under engine/WgpuRenderer/docs/ (checked 2026-08-14)) |
| Volumetric sun shafts (god rays / crepuscular rays) | **Works** | NO PLAN DOCUMENT EXISTS. | 2026-08-14 | [plan](../../../# NONE — no god-rays plan document exists under engine/WgpuRenderer/docs/ (checked 2026-08-14)) |
| Road / path rendering (terrain-conformed on-surface geometry) | **Partial** | HISTORICAL PRE-PERF-004 DEFECT, NOW SUPERSEDED — the Arma 2 / OA road banding (WLD-020, WLD-021). | 2026-08-14 | [plan](../../../engine/WgpuRenderer/docs/terrain-conform-vegetation-roads-plan.md) |
| Modern-world WGPU terrain, object, model and texture residency | **Works** | Rust model/LOD/section metadata is append-only when a streamed C++ model is destroyed and later re-registered. | 2026-08-14 | [plan](../../../docs/roadmap/decisions/PERF-004-wgpu-modern-world-residency-and-terrain-20260814.md) |
| High cirrus cloud deck (second cloud layer) | **Works** | NO PLAN DOCUMENT EXISTS, and specifically it is NOT covered by procedural-sky-plan.md — that plan's Stage 5 is the cumulus deck, and the string "cirrus" does not occur in it at all. | 2026-08-14 | [plan](../../../# NONE — no cirrus plan document exists; procedural-sky-plan.md never mentions it (checked 2026-08-14)) |
| forward-plus | **Unavailable** | No implementation on this branch. | — | — |
| gpu-terrain-water-cull | **Unavailable** | No implementation on this branch. | — | — |
| terrain-fractal-detail | **Unavailable** | No implementation on this branch. | — | — |

## Build-truth tickets

These are the Preview-0 release blockers. `VALIDATED` here means the integration
owner verified it, not that a second party did — the project has one human, so
every review is owner-performed and each ticket records what was actually
exercised. That distinction is the honest reading of every row below.

| Ticket | Title | Lifecycle | Reviewed by |
| --- | --- | --- | --- |
| `PERF-001` | Platform and performance targets | VALIDATED | Oliver Kay |
| `CORE-005` | Authoritative machine-readable status ledger | VALIDATED | Oliver Kay |
| `CORE-NEG-001` | Reproducible WGPU build and startup | VALIDATED | Oliver Kay |
| `CORE-NEG-002` | Versioned C++/Rust ABI | VALIDATED | Oliver Kay |
| `RND-005A` | Renderer startup, capability, and safe shutdown | VALIDATED | Oliver Kay |
| `TEST-002` | Capture, metrics, and build fingerprint | VALIDATED | Oliver Kay |
| `REL-000` | Preview release package and public capability matrix | INTEGRATED | not yet reviewed |

