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



## Follow-up validation and limits

Oliver clarified that native Reforger compatibility is incomplete (material/shader

metadata anomalies and missing furniture). Treat it as a load/cancellation fixture

and a same-build change detector, never as a ground-truth visual corpus. The DayZ

ChernarusPlus packed OPRW corpus is now also included, at his dense location

(5704.58, 2523.37, 73.74), azimuth 260.9, elevation -3.8. The runner also provides

an original CWA control (`-Corpus Legacy -Normal -Identity`).

`a531d7b9` closes an ownership-accounting gap between parsing and install: queued,

active and completed conversions keep the IR charge plus an explicit equal-sized

workspace/output reservation until consumed/cancelled. Old-generation active jobs

retain their charge until completion. This is a scheduling estimate, **not** actual

Shape allocation or decoder scratch measurement. Useful conversions are never

blocked by this debt; speculative parsing is. Five worker tests / 139 assertions

cover consumption, cancellation, reset and oversize progress.

The first adaptive-detail run on `7d8fb75b` FAILED visual acceptance despite matching

logical object counts: image-handle changes triggered whole retained-model teardown.

The same-slot material remained valid, but buildings/fences disappeared. `a531d7b9`

keeps these registrations for the narrowly eligible opaque replacement path.

The repeat has zero texture-caused model re-registrations and restores the missing

geometry in the endpoint capture. This demonstrates why resident-object counts

alone are insufficient coverage evidence. Both runs remain performance failures:

per-pose p95 frame times reach roughly 90-97 ms versus 25-36 ms with detail disabled.

No FPS gain is claimed from their lower medians. Optional detail remains OFF.

### Conventional geometry audit (F)

- XOB conversion (`XobConvert.hpp`) visits visual LODs, maps authored thresholds,

  validates monotonic resolutions, and handles special geometry separately.

- Model IR / ShapeAdapter retain visual and query-purpose distinctions; renderer

  registration in `EngineWgpu::RegisterGpuModel` skips special levels >=900 and

  appends each eligible drawable level with its resolution and section range.

- Rust `CullState::register_model` stores all submitted LOD/section records; compute

  `cull.wgsl` selects a level per instance. Selection does **not** free other LOD

  payloads. Geometry pool capacity also differs from live geometry bytes.

- Existing retained/direct mesh sharing is already enabled; CPU complements keep

  proxies, animated/non-owned sections and supported legacy foliage/window fallbacks.

  Removing these would lose content, not optimize equivalent rendering.

- The comparable native endpoint uses about 624.5 MB live geometry within 704.6 MB

  pool capacity, alongside about 1003 MB object textures and a 5884.5 MB reported

  budget. This does not demonstrate a geometry-residency capacity crisis. Shadow

  rasterization remains a large GPU cost; it is not proof that page streaming helps.

- Do not schedule optional Stage G's hierarchy until conventional geometry costs

  establish a benefit. No content decimation, collision LOD changes, mesh-shader

  requirement or GL33 work is justified by these measurements.

Stage D's DayZ cold-model rows demonstrate that packed models still take the

synchronous fallback. An example `misc_range_roof` costs 275 ms, of which about

269 ms is texture work (34 ms read, 236 ms create); moving its ~4 ms model parse to

workers alone cannot fix that stall. The unlocked file-server MRU must stay on its

owner thread. This is attribution, not a claimed fix or an excuse to disable assets.



DayZ `dayz-dense-20260920-104925` on a531d7b9 is a **failed** runtime run:

at 10:50:57 the GPU reported Out of Memory while vertex capacity grew from

544 to 1088 MiB. Subsequent timings/screenshots are invalid performance evidence;

the process did not complete clean shutdown inside the test timeout. The report's

275 ms roof example above was after this failure and is withdrawn as performance

attribution. Earlier cold rows remain useful leads only. Do not combine pre/post

failure timings. The camera identity probe reports 16/16 IDs missing while away

and restored on return; this agrees with source ownership but does not establish

full query/collision correctness, and must be repeated in a clean run.

The pool already has a measured growth-factor lever (`WGR_GEO_POOL_GROWTH`,

default 2, range 1.25-2); do not invent a duplicate growth policy. Next compare its

existing smaller-growth option and actual retirement/overlap accounting without

reducing the object budget or sight distance.

The subsequent native run `detail-idle-20260920-105734` on 21b89e79 uses a
nonblocking producer window at frame start, with pending model registrations
excluded. It performs 35 replacements (17 promotions), with no texture-triggered
mesh teardown and the expected endpoint buildings/fences present. Per-pose frame
p95 values are 34.00 / 24.82 / 34.04 / 24.76 ms, versus the original OFF run's
35.92 / 24.57 / 35.68 / 25.55 ms. Some maxima remain around 97-99 ms; this is not
a universal no-regression claim and not grounds for default activation. The narrow
prototype now avoids mandatory render-thread waits; general texture streaming and
secondary-view/flight quality acceptance remain open.

082eaa06 additionally counts retired geometry bytes until the queue-completion
callback fires. The seven GPU pool tests pass, including actual surviving-vertex
readback across growth/shrink and completion-based retirement. Combined C++ model
LOD/XOB/preparation/cancellation tests: 1092 assertions in 52 cases pass.


### Additional stress evidence

Reforger native compatibility is incomplete (including material metadata and
building furniture). Its captures establish same-scene workload behavior, not
asset correctness. DayZ ChernarusPlus at 5704.58 / 2523.37 / 73.74, azimuth 260.9,
elevation -3.8 supplies an independent dense corpus.

`dayz-growth15-20260920-110626` on 082eaa06 completed cleanly with the existing
WGR_GEO_POOL_GROWTH=1.5 override. First full set was 19923 objects after about
39.4 seconds; four pose p95 values were 26.61 / 31.29 / 24.33 / 21.35 ms.
The whole-device 1 Hz monitor peaked at 7765 MiB; this includes other applications.
The failed factor-2 run had no matching GPU monitor, so this is not a paired
memory/performance improvement claim. No default growth-factor change yet.
The clean identity probe lost all 16 selected IDs away from the camera and found
them again on return. Full logical/collision independence remains unresolved.

`pressure-fill-20260920-111148` exposed a preparation deadlock at the deliberate
1 MiB ready budget: one redundant prepared model retained 144238 bytes while
733 placements waited indefinitely. A proxy/other admission can load the shape
before its queued copy is consumed. Warm admissions skip the preparer, so that
copy must be retired independently. c2c8eb0b reclaims ready IR copies and completed the reproduced fill to 8000
objects with zero remaining preparation reservations. The full test, including
stationary shrink/restore and away/return, passed. Completed converted copies
will receive the same redundant-result treatment as a lifetime safeguard. The c2c8eb0b deployment carried only uncommitted audit text
(the +dirty stamp), not uncommitted runtime code.


### Allocation-pressure experiment (not default)

WGPU internal counters now expose backend buffer + texture allocation bytes
separately from streamed payload totals. These include fixed render resources
and staging, but exclude allocator slack, driver-private overhead and other
applications; zero means unavailable. Counters are native backend observations,
not a claim of portable exact VRAM usage. WgrMemoryStats joins both ABI layout
hashes and the contract advances to version 11.

WGR_GEO_POOL_PRESSURE_GROWTH=1, with WGR_GEO_POOL_GROWTH unset, keeps ordinary
factor-2 growth below the soft allocation budget and limits growth slack to 1.5
when current backend allocations plus the complete new buffer exceed it. Current
allocations already include the old buffer. Every pending required allocation
still fits; this neither reduces authored geometry nor guarantees a hard memory
cap. Explicit factor overrides and the default OFF path remain unchanged.
Use the existing runner with -GeoGrowth 0 -PressureGrowth to evaluate it.

The clean DayZ factor-1.5 run contains substantial texture stalls: for example,
the initial city_school row totals 1194.2 ms with 771.1 ms texture work (602.8 ms
read, 43.9 ms create, 124.4 ms alpha); later city_school reload is 1419.2 ms.
These are valid clean-run leads. Moving only model parsing to workers cannot
remove the measured read/upload/renderer-window component. Packed file-server
MRU access remains on its owner thread.

The artifact checksum manifest is `docs/roadmap/evidence/streaming-20260920.json`.
