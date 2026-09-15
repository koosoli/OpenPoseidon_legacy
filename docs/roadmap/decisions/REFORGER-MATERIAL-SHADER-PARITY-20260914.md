# Reforger material/shader parity: audit and investigation plan

Date: 2026-09-14. Owner brief: match native Reforger rendering by fixing
material/shader/lighting semantics, never by artistic fudge factors.

## 1. Scope and provenance

- Active work line: `snowline-jitter-001` @ `12061d7a` (worktree
  `.claude/worktrees/farvisible`). Installed game build is this commit
  (stamp verified). Trunk push (`ship-trunk` merge into
  `safety/consolidated-v2`) interrupted mid-build, NOT pushed, parked.
- Reference: owner screenshots of original Reforger vs OpenPoseidon at
  freefly [4665.50, 7132.80, 192.09] az 123.7 el -11.8, 16:00 (Everon
  church/village). Geometry placement matches; vegetation/buildings/
  terrain/atmosphere response differs strongly.
- Other-AI worktree `.claude/worktrees/asset-obs` (branch `asset-obs-001`
  @ `7d871e92` + uncommitted changes) reviewed read-only below. Nothing
  there was modified. Its committed VBSmallDiscardable result (path
  inactive in tested scenes, diagnostics only) is accepted as evidence;
  its uncommitted native/far/culling work is judged hunk-by-hunk below.
- Prior art consulted (not re-litigated): `UPSTREAM-CROSS-AUDIT-20260913`,
  `HANDOVER-20260815-reforger-renders`, `HANDOVER-20260823b-everon-renders`,
  `HANDOVER-20260826`, `compat-c1-capability-matrix`, `SEPTEMBER9-VISUAL-
  FOLLOWUP` (evidence index).

## 2. Prior measured findings (do not re-guess these)

- `BCRMap` = albedo, `OpacityMap` = cutout. Reforger `NTCMap`/`NMOMap` are
  NOT plain tangent normal maps and are still unwired
  (`HANDOVER-20260815-reforger-renders:280-295`).
- WGPU terrain ignores rvmat `ambient`/`diffuse`; measured albedo-to-pixel
  transfer is ~3.0x and compressive (same source).
- Granite albedo measured identical before/after material work while the
  image stayed wrong: "start at the lighting/exposure path, not the
  materials" (`HANDOVER-20260823b:261`).
- `EmatMaterialAdapter` does NOT map `GeometryAO*`, `EllipsoidAO*`,
  `WorldColorizeMap`, SSS (`UPSTREAM-CROSS-AUDIT` Reforger section).
- Forest-leaf-light ablation did NOT solve pale foliage; global legacy
  foliage-light ablation must not ship (same source).
- BCR alpha is roughness (AST-016 series); DayZ `TreeAdv` colorization
  binds raw albedo; RVMAT-`ambient`-applied-literally tested (see handover
  20260815-next-session:120).
- Auto-exposure default-on; suspected exposure regression vs `aa24c32`;
  Hable curve at exposure 3.625 noted (`HANDOVER-20260813:182`).
- God rays driven by sky Mie + overcast + fog, 1200 m aerosol falloff.
- Baked-LOD `MatPBRBasic` flat-`Color` materials deploy a colour-only
  material (MAT-054).

## 3. Core question A-M: initial status (evidence, not verdict)

| # | Area | Status | Evidence / next probe |
|---|---|---|---|
| A | Texture channels | PARTIAL (BCR/Opacity/NHO-ok; NTC/NMO unwired) | wire NTC/NMO per actual convention, verify sRGB/linear per channel |
| B | Material semantics | PARTIAL (adapter gaps above) | map or explicitly reject GeometryAO/EllipsoidAO/WorldColorize/SSS |
| C | Normals | UNKNOWN (suspect legacy rounded-foliage leak, handedness) | Phase 1 normal views on control tree |
| D | Foliage shading | INCORRECT (pale, flat; ablation failed) | Phase 3 isolation ladder |
| E | Roughness/specular | PARTIAL (BCR-alpha known; response unverified) | specular debug view on control assets |
| F | AO | PARTIAL (ground NHO ok; authored _AS: no path) | count AO applications (0/1/2) via views |
| G | Sky/environment light | UNKNOWN (ambient suspected too strong) | sky-only vs sun-only views |
| H | Direct sun | UNKNOWN (color/intensity unverified) | sun-only view + albedo reference |
| I | Shadows | PARTIAL (CSM + contact work recent; darkness unverified) | shadow-term view |
| J | Atmosphere/haze | UNKNOWN (froxel exists; distance response unverified) | fog-contribution view + range ramp |
| K | Exposure/tonemap | SUSPECT (3.0x compressive; regression suspected) | pre/post-tonemap views, fixed-exposure A/B |
| L | Terrain blend/detail | PARTIAL (LCA + native ground exist; macro/detail normals unverified) | Phase 5 layer isolation |
| M | LOD materials | PARTIAL (flat-Color handled; far tiers unmeasured) | compare LOD ladder per asset |

## 4. Asset-obs work review (read-only diff vs `snowline-jitter-001`)

Committed diagnostics (VB lifecycle census, `7d871e92`) already in ancestry:
ACCEPTED as evidence. Uncommitted copies of my sim/snow files: stale,
identical-or-older, IGNORED.

ADOPT (Phase 1 debug-view stack + safe fixes; no behavior change at defaults):
- `frame.wgsl`: `veg_sway_phase` (legacy `leaf>=0` path bit-identical) +
  `MATDBG_VIEW_* 0-6` stable IDs.
- `gpu_driven.wgsl`: matdbg views 3-6 (AO/spec/gloss/UV), specular_debug,
  constant-ID renumber (default FULL falls through). Sway line excluded.
- `shader3d.wgsl`: global matdbg with per-draw-lane fallback (default FULL
  = old behavior), specular constants, UV view (blue=0, no uv1 lane).
- `gfx3d/mod.rs`: shared-ID test only.
- `shaders/mod.rs`: `veg_sway_phase` asserts (gpu assert excluded, see below).
- `EngineWgpu.hpp`: 1:1 rawView passthrough (fixes unreachable views 3-6).
- `Engine.hpp`, `ffi.rs`, `lib.rs`: comment-only ID documentation.
- `DebugOverlay.cpp`: views 0-6 combo + honest Screen-AO help.
- `EngineWgpu.cpp`: debug env aliases + drain timing/counters logging.
- `ObjectStreamPrepare.cpp`: native-gated EMAT texture noting (early-return
  branch preserves loose-PAA path).

DEFERRED (needs measurement or out of scope for Phase 1):
- RFG-104 far-proxy tier, authored-far residency, satmap build+bind,
  far-distance follow, `SceneFarObjectCreated` path, `far.wgsl` sun flip,
  cull.wgsl sub-pixel 0.5 + bias bit + `authored_far_level`, gpu sway line,
  terrain.wgsl native crossfade distances, MODERNISATION doc delta.
- Rationale recorded per hunk in session notes; each needs its stated
  capture (coverage/cost/pop-in) before adoption.
- Explicitly NOT ported: hardcoded fallback colors/palette defaults stay
  out; hardcoded coverage/perf knobs stay out.

## 5. Phase 1 plan (in progress)

Port the ADOPT set above into `snowline-jitter-001`, adjusting:
- `shaders/mod.rs`: drop the `gpu.contains("veg_sway_phase(phase_xz, leaf)")`
  assert (deferred sway line), keep the rest.
- `gfx3d/mod.rs`: port only `material_debug_views_are_shared_by_both_object_paths`;
  skip `authored_far_lod_preserves_small_coarse_silhouettes` (asserts deferred cull).
Then: Rust gfx3d tests, full build, deploy, verify all 7 views on the Everon
church spot via fixed-camera captures (views must show distinct, sane output;
FULL must be pixel-comparable to pre-change).

Pre-existing failure (NOT introduced by this port, NOT fixed here):
`gfx3d::normal_map_cavity_only_darkens_the_ambient_and_is_inert_on_a_flat_map`
fails at base `12061d7a` because `shading.wgsl:501` ends the sun term with
`* sun_vis * leaf_cavity` while the test requires `* sun_vis;` — i.e. the
shader currently touches the direct sun term the test forbids, on the foliage
path. Neither file is touched by the Phase 1 port (verified via `git status`).
Flagged as an open Phase 3 (foliage) question: is `leaf_cavity` on sun
intended or a regression? Do not weaken the test to make it pass.

## 6. Control assets (selected after Phase 1 verifies)

- Building: church at the Everon comparison spot (visible in both screenshots).
- Tree: large deciduous tree near the church (foreground right in ours).
- Exact asset/material IDs to be recorded here once the debug views are live.
- Trace per asset: source material -> loader/importer -> internal material ->
  CPU state -> GPU bindings -> shader -> framebuffer.

## 7. A/B route (to be fixed before Phase 2 captures)

Fixed camera/pose/FOV/time-of-day(16:00)/weather/exposure/resolution/AA/scale
via existing capture harness (`farfield-bench`/movecam scripts + fixed LOD
governor). Views: building close-up, tree close-up, village mix, mid vegetation,
long landscape. No hand-positioned cameras.

## 8. Guardrails (from owner brief, enforced)

- No darkening textures, no ambient cuts, no AO boosts, no sun/exposure tweaks
  to match screenshots. No source-texture edits. No baked per-asset factors.
- Native-only branches keyed on asset/material origin; legacy OFP/Arma1 output
  covered by existing regression tests plus before/after captures on a legacy
  scene for every rendering change.
- One mismatch per change: demonstrate, cause, test, fix, capture, verify.
- Perf: mean/median/p95/p99 over repeated runs; no FPS claims from single samples.

## 9. Open questions / next actions

1. Execute Phase 1 port + verify views (this session).
2. Select control assets, run Phase 2 trace.
3. Strongest pale-vegetation suspects to test first: sky-ambient strength,
   legacy rounded-foliage normals on native assets, NTC/NMO unwired (flat
   shading), exposure transfer. In that order only if views implicate them.
4. Trunk ship (`ship-trunk` -> `safety/consolidated-v2`) remains PARKED until
   the owner reprioritizes; installed build stays `snowline-jitter-001`.
