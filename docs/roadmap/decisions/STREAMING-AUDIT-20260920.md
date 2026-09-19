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

Runtime results will be recorded after the controlled reproduction and correction.
