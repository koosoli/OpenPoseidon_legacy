# Streaming execution audit — 20 September 2026

Baseline: `03e713714b9b699e3f6f4eae7cdce7f488a13b64`, previously
`codex/ocean-release`, now `codex/streaming-residency`. The dedicated worktree is
`.claude/worktrees/combat-audit`; other checkouts are not integration targets.
The wave prototype is parked on GitHub remote `safety`, branch
`codex/backup-waves-20260920`. The old ocean remains default.

## Verified ownership and routing

| Area | Current owner and result |
|---|---|
| Residency invocation | `World.cpp`, before `CapturePresentationSnapshot`, inside `enableDraw` and renderer capability/camera checks. Already moved from TerrainWgpu, but still not a headless simulation service. |
| Native/modern placements | `LandSave.cpp`: retained placement table, cell desired set, main-thread admission and release. Original legacy OFP maps keep their existing lifecycle. |
| Eviction | Clears resident link, removes Object and ID map entry; retains shape only. Native far destruction bit is not full mutable-state persistence. Geography accumulation is monotonic and does not establish collision residency. |
| Preparation | `ObjectStreamPrepare`: existing loose/native archive model loading, compatible adapter workers, generation cancellation. Shared archive MRU/shape-bank mutation ownership must stay explicit. |
| Texture preparation | Existing PreparedTextureStore budget and generation tracking. No second cache needed. Decode transients still need distinct accounting. |
| Textures/geometry | Existing lazy texture uploads, stable bindless leases/pins and shared retained meshes. Mip feedback is telemetry, not per-texture promotion/demotion. |
| Terrain/water T0 | `EngineWgpu.cpp` constructs TerrainWgpu unconditionally and WaterWgpu by default. `LandscapeRender.cpp` returns after modern opaque-ground/water submission; legacy segments remain for grass and explicit water fallback. No duplicate modern/legacy opaque pass to delete. |

## First reproduced-defect target

`UpdateModernObjectResidency` returns on unchanged center/no pending admission
before evaluating the optional GPU budget. Candidate growth depends on budget,
but the cache key omits it; desired selection and release only follow center changes.
No other invalidation path was found. The harness-only `stream_residency` command
and `Test-StationaryResidency.ps1 -Baseline` isolate this on native Everon using
an explicit 8,000→4,000→8,000 count injection. This reduced-budget run is a
correctness diagnostic, not a normal-settings performance benchmark.

## Delivery gates

- Keep the historically oscillating GPU pressure estimator disabled by default.
- Do not broaden camera-driven logical-object destruction to legacy maps.
- Do not interpret fewer resident objects as a performance win.
- Adaptive detail and hierarchical paging require evidence beyond this first
  invalidation correction; geometry paging is not justified merely by this plan.
- Headless/native simulation interests, distant query geometry and full mutable
  state retention remain unresolved prerequisites for aggressive eviction.

## Verified increments (work remains)

- `7d5f3e71`: controlled installed reproduction. At fixed cell (404,316), reducing
  the budget 8,000 to 4,000 left desired/resident at 8,000, with no pending work.
- `f733e708`: budget invalidates both candidate growth and desired/release sets.
  Installed test: 8,000 to 3,987 to 8,000; center unchanged. Settled idle admission
  count stays fixed. Controller remains opt-in. Reset clears budget/cache state.
- `8e09f391`: 512 MiB soft ready-IR payload limit, 16 MiB unknown-parse reservations,
  explicit oversize progress and old-generation reservation retention. The byte
  estimate includes vector capacity, not just used vertices; excludes decoder
  scratch, converted Shapes and allocator overhead. These are deliberately not
  claimed as a total CPU-memory bound. `WGR_OBJECT_STREAM_READY_MB=0` is the A/B.
  106 assertions in four real preparer/cancellation tests passed. Installed normal
  Everon route: peak ready payload 182,807,545 bytes; no byte-limit parks;
  19,999 desired/resident objects before and after return.

Initial 12-second screenshot-free stationary samples: 8k baseline/fix GPU medians
18.6575/18.523 ms; 20k baseline/fix 21.9075/21.9695 ms. Six GPU log samples each.
The first moving comparison varied (25.3095/27.53 ms); wall-clock stepping changes
how long asynchronously admitted geometry is visible during a leg. This is not
accepted as proof of no moving regression; repeat with controlled per-pose coverage.

## Remaining gates

A: camera/ID/geometry probe, modern OPRW and legacy corpus checks, controlled movement comparison.
B: change during unfinished fill, repeated same-demand controls and documented simulation-pinning limitation.
C: conversion/decoder transient accounting and resource-aware pressure policy; ready IR alone is not all memory.
D: profile uncovered synchronous work. Existing blanket DDS-worker experiment previously showed
no stable gain; do not enable it merely because the worker exists.
E: validate replacement lifetime and feedback identity/age before opt-in adaptive detail.
F: finish authored LOD/allocation audit; G only if a measured bottleneck justifies it.
T0: modern opaque terrain/water routing already present; no speculative wrapper removal.

Texture-lifetime prerequisite: a new headless GPU test reproduced an old-handle
retirement clearing a newer image in the same leased slot (green became white).
The slot now clears only if it still refers to the retired image. Six texture
lifecycle tests pass. Feedback now records capture age, rejects older out-of-order
results, and invalidates observations on slot/image replacement; its identity test
passes. Retired texture payload stays counted until queue completion (not a guessed
frame delay). C++ streaming tests: 416 assertions pass, 29 cases pass and one
native-DDS opt-in case is explicitly skipped.

An opt-in `WGR_ADAPTIVE_TEXTURE_DETAIL=1` prototype targets opaque in-memory DDS BCR
object textures. It leaves cutouts, normal maps, composites, UI and terrain alone.
It uses conservative full-detail demand on camera/projection changes or unknown
feedback, delays demotion, retains a valid image, and caps each replacement at
4 MiB with old/new/staging headroom. It is not yet runtime-accepted and remains off
by default. General adaptive texture rollout is not claimed.
