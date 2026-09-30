# Grounded rigid mesh puddles — source candidate

Base `06a7c41e`; isolated branch `codex/ground-mesh-puddles-20260930`.

The owner's any-ground request previously covered terrain and pinned direct OnSurface roads. This change extends it to a bounded subset of ordinary direct/retained rigid meshes. It does not certify every ground mesh or map. Installed appearance and producer coverage remain pending.

## Source and fragment admission

The producer proves a static intact world owner, owned visual mesh, rigid current geometry and opaque depth-writing/cutout source. Primary objects and actual Building/Thing owners may qualify. Person, parked ordinary vehicles, child proxies, configured geometry animations, procedural Church/Fountain owners, vegetation, forest planes, pinned On vertices, cockpit, monitor, reflective and glass paths remain excluded. Direct dynamic allocation hints require the existing bounded exact current-section position/normal proof without the snow-weather prerequisite. A successful snow proof is reused. Retained models use the existing loaded-visual/current-pose proof, recalculated at registration/update, plus a bounded all-visual retained-section check for actual source depth-writing, no glass and no leaf-card material. Unknown or mixed unsupported retained sections conservatively reject the model's ground tag; direct complement sections prove themselves separately. Proxies never inherit the tag.

Private retained location13 carries bit0 snow and bit1 ground with independent bit checks. Draw bit4096 and instance bit256 change semantics only. Public structs, shader varying count, geometry, culling and wind are unchanged. Paging hide/replacement and actual receipt/equivalence masks preserve both benign receiver bits while rejecting unrelated flags and mismatched original/page tags.

Every candidate colour fragment needs a valid authored upward normal (normalUp >= .995), the viewer on its authored outside side, and height within .15m of the exact two-triangle **bare** terrain height at that fragment's world XZ. Disabled/malformed maps, dimensions mismatch and positions outside complete terrain cells fail closed. Missing data cannot license height zero. No grounded corner or owner AABB licenses a complete roof or a triangle spanning a hillside. Existing fragment heightmap bindings0/1 suffice: this helper never reads SnowData storage2 and does not grow the retained eight-storage-binding budget.

Bare height intentionally excludes Mud/Sand depression: rigid paving does not sink with soft soil. Existing surface_y factors the same base interpolation and still adds both offsets. Mud/Sand numeric GPU tests are unchanged; their source-call assertions recognize the factored helper.

Fragments use the existing world-fixed puddle mask, simulation wetness, filtered liquid-rain ripples, inland/slope/snow and physical zenith cover gates. Alpha/discard/A2C/prepass silhouettes and depth remain unchanged; film modifies colour shading. Direct ground uses actual sampled snow depth. Retained ground cannot sample SnowData within its fragment binding budget: it conservatively uses frame deposit plus existing object-snow altitude/cover admission. The object-snow diagnostic master-off switch also zeros frame deposit, so terrain-snow exclusion under that diagnostic combination is not certified. Missing/outside physical weather coverage stays dry.

Optional `WGR_GROUND_RECEIVER_TRACE=1` reports at most64 distinct model/path/source-admission rows with world positions. Logs explicitly say fragment height/side/rain proof remains; source telemetry does not certify pixels.

## Evidence and remaining checks

Passed locally: `scripts/Test-GroundMeshSurfacePolicy.ps1 -Compiler 'd:/Program Files/LLVM/bin/clang++.exe'`, C++20 Wall/Wextra/Werror unchanged. It exercises actual page-instance, pair-transaction and stable-frame receipt constructors with neither/snow/ground/both tags; takeover, restore and page update; tag-drop/mismatch and unsupported wind/vegetation refusals. It checks production shader routing, lane independence, map/side proof and all changed residency masks. Rustfmt and git diff whitespace checks passed.

Authored, **not run here**: `cargo test -p wgpu_renderer ground_mesh -- --test-threads=1`. It composes actual helpers and both complete entry shaders. The GPU falsifier requires a real device and production helper, non-planar 2x2 height texture and a resource layout without SnowData: both triangle heights, nearby paving, raised roof, large flat surface above sloping terrain, underside, zero/steep/down normals, disabled/malformed grid/dimensions and out-of-map points. Root owns GPU execution, full suite/build and matched installed deployment. No game/GPU/renderer/full-game build ran in this worktree.

Installed proof still requires exact source model/pose and matched binary provenance for exposed stock pavement, raised roof/wall negatives, covered ground floor, cutout holes, drying/pause and camera return. Existing controlled paging takeover/restore must preserve the receiver tags. Source admission and terrain stills alone cannot certify this expanded mesh scope.
