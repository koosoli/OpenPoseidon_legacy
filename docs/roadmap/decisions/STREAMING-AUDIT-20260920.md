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
| Eviction | Baseline clears resident link, removes Object and ID map entry; cd3d5c68 now retains explicitly required identities and MustBeSaved state with separate visual retirement. Native far destruction bit is not full mutable-state persistence. Geography accumulation is monotonic and does not establish collision residency. |
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

## Current gates (updated after the implementation runs below)

- A/B: stationary shrink/restore, in-flight changes, unchanged-demand idle path,
  native/DayZ/legacy coverage and read-only ID probes are verified. Explicitly
  required identities and MustBeSaved state survive visual retirement. An actual
  required-object fire obstruction survives a 1500 m camera departure. Never-
  requested placements still disappear; actor-neighbourhood collision/navigation,
  view queries and multiplayer independence are NOT established. Same-world and
  fresh-process client save/reload of one required damaged obstruction are verified.
- C: ready IR and active scratch reservations, real backend counters and GPU
  retirement are implemented. Model/Shape allocator costs remain estimates;
  automatic allocation-aware growth remains experimental after mixed timing.
  PreparedTextureStore charges retained vector/mip metadata capacity; map/string
  and allocator overhead remain excluded.
- D: incremental sky uploads and model-lighting retirement are integrated.
  Owned PBO model/PAA preparation is implemented but OFF pending comparisons.
  Late material stages, texture uploads and alpha classification still cost time
  on the owner thread; a nonblocking parser does not bound the entire frame.
- E: lease-safe whole-image replacement and conservative feedback selection are
  opt-in. Native timing is mixed; secondary-view/visual acceptance is incomplete.
- F: authored LOD/conventional path has been audited/tested. Geometry capacity
  and growth overlap matter; a selected coarse draw does not release fine LODs.
  No hierarchical page implementation is promoted on this evidence.
- G: conditional experiment remains unselected while the above conventional
  lifetime/preparation costs are addressed.
- T0: modern opaque terrain/water delegation already returns before the legacy
  loops. No speculative wrapper removal or GL33 work is warranted.

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

No FPS gain is claimed from their lower means. Optional detail remains OFF.

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


### f602332c runtime results

The committed build passed 8 Rust pool tests, 13 ABI tests and 139 C++ assertions
across five preparation/cancellation cases. The installed pair is stamped
f602332c; the ABI handshake succeeds with the memory structure in its layout hash.

`dayz-pressure-growth-20260920-114119` completed full 20k-budget traversal and
1500 m away/return without renderer errors. The actual growth decision used
5,533,293,728 allocated bytes against a 5,884,503,654-byte soft budget, choosing
544 -> 816 MiB vertex capacity instead of 544 -> 1088 MiB. No geometry/object cap
was reduced. First full set: 19923 placements after 240 admission updates.
Whole-device peak: 7770 MiB. Per-pose frame mean/p95/max in ms:
20.96/22.86/49.92, 23.86/28.39/49.93, 13.61/22.06/104.64,
20.52/35.91/97.20. Endpoint screenshot inspected: expected street buildings,
walls, trees and wreck remain present; this is comparable coverage, not a claim
that all DayZ materials or placements are correct.

The same-build, equally instrumented factor-1.5 control
`dayz-matched-growth15-20260920-114805` also completed cleanly. First full set
again took 240 updates. Whole-device peak: 7832 MiB. Pose times:
22.98/29.54/44.78, 20.56/24.00/105.91, 15.39/22.56/133.28,
19.64/22.25/28.42. The automatic arm has a worse last-pose p95; do NOT advertise
universal improvement or enable it by default on this evidence. Both controls
remain available without modifying global graphics settings.

`legacy-control-20260920-114612` completed normally, never entered the modern
streamer, and retained all 16 selected object IDs before/away/after. Native and
DayZ lose the corresponding selected IDs while away. This confirms the lifecycle
boundary rather than validating camera-independent simulation in modern worlds.

### Next measured CPU slice: owned packed-model reads (experimental)

The selected DayZ residential PBO contains 299 uncompressed P3D members,
including City_School (2,341,505 stored bytes). The existing async preparer
refuses packed paths, forcing parser and adapter work onto the owner thread.
WGR_OBJECT_STREAM_PBO=1 snapshots uncompressed member locations on the owner
thread and reads them using private worker streams. It reuses the existing P3D
parser, worker conversion, queue limits and generation rejection. Compressed,
encrypted, patched or oversized members retain the normal VFS fallback. No
worker touches QFBank seek state or the file-server MRU. The snapshot path is
OFF by default until its corpus/performance comparison passes.


### Packed-model runtime and sky-atlas follow-up

`dayz-packed-models-20260920-120708` on dca4f954 FAILED with renderer
OutOfMemory at 12:08:58.941; subsequent timings are invalid. The worker path
captured 2352 packed-model requests and moved parsing/adaptation off the owner
thread, but factor-1.5 geometry growth still coincided with sky_volume_data
128 -> 256 MiB growth. Packed preparation remains OFF. The paired parser tests
passed (155 assertions / 6 cases including cancellation); private PBO read
concurrency and bounds passed separately (14 assertions / 1 case).

Inspection found that each added sky volume recopied and restaged the entire
atlas. Incremental uploads retain stable voxel offsets, upload only new volumes
and metadata, and copy the GPU prefix only when capacity grows. Lighting samples
and gates are unchanged. WGR_SKY_VOLUME_INCREMENTAL=0 retains full-upload control;
runner -FullSkyUploads records this setting. A real GPU readback test verifies
prefix preservation, new writes and zeroed missing metadata through growth.
Runtime comparison is still pending; this does not prove the OOM resolved.


### 0d6a498f atlas validation and remaining pressure failure

The GPU prefix-copy/readback test passed. `dayz-sky-incremental-20260920-122407`
(PBO workers enabled) still failed with OutOfMemory during return traversal;
reducing atlas staging alone is NOT the complete memory fix. No post-error
measurements are accepted. PBO workers remain opt-in.

`dayz-sky-default-20260920-122632` (existing synchronous PBO model path,
factor 1.5, incremental sky atlas) completed the controlled route with
19923 placements before/after, no renderer errors, and an inspected endpoint
capture retaining the street/building/tree/wreck coverage. Whole-GPU peak
7783 MiB; backend allocation peak 5,855,836,000 bytes. Per-pose frame mean/p95/max:
19.65/21.17/69.11, 20.06/21.24/112.55, 17.43/28.02/201.45,
18.88/22.52/117.80 ms. Long-tail frame stalls remain. A full-upload control on
the same build is being measured; this is not yet a universal performance claim.

The next lifetime correction records the actual texture slot lease when baking
model materials. A shared texture reuploaded by another model can change its
image handle without changing the baked slot. Such models need no geometry
re-registration. A newly acquired lease after a previously unleased upload is
NOT accepted as proof; null/missing/moved slots retain the old rebuild path.


Same-build `dayz-sky-full-control-20260920-122922` also passed, with the same
19923 before/return coverage. Full-upload control frame mean/p95/max in ms:
19.25/20.72/125.72, 20.78/23.22/125.88, 17.18/27.32/39.72,
20.82/22.28/24.81. Whole-GPU peak 7758 MiB. These settled measurements are close
and maxima are noisy; they do not establish a broad FPS improvement. The atlas
change removes repeated full-atlas CPU/staging copies by construction, preserving
GPU data in the readback test and scene coverage in the route. Remaining pressure
failures are reported independently, never labelled fixed by this change.


### 82cbb5a2 shared-slot verification

Build and three existing GPU slot lifetime tests passed. The packed-model DayZ
route `dayz-shared-refill-20260920-123531` completed without renderer errors,
with 19923 placements before/after and zero queued/reserved preparation work.
Frame mean/p95/max: 19.85/21.32/22.84, 19.79/21.01/73.97,
13.36/24.07/131.05, 18.91/20.93/26.81 ms. Whole-GPU peak 7798 MiB.
The shared-refill counter did NOT fire, so this run does not establish that
shared refills caused the earlier OOM or that this correction resolved it.
Worker ordering and memory peaks remain variable; packed preparation stays off.

Next measured ownership boundary: C++ releases model meshes, but the Rust
registration previously had no matching retirement, leaving CPU sky volumes,
GPU atlas slots and pending sky bakes behind. Explicit FIFO model retirement
now cancels pending bakes, frees the CPU payload/budget, reuses fixed-size atlas
slots, clears retired metadata and empties the retired model's LOD draws.
IDs are not reused; geometry ranges retain their existing queue-completion
lifetime. Atlas capacity is retained for reuse, not claimed as freed VRAM.
ABI version 12 requires a matching executable/DLL pair. Validation pending.


### 486d5e9b model retirement validation

The GPU cull end-to-end test now retires a visible model while leaving its
instance behind, repeats retirement and passes an invalid ID, then verifies
zero indirect draws. It passed, as did six ABI contract tests. The paired build
completed `dayz-model-retirement-20260920-124602`, including controlled traversal
and the 1500 m away/return. Before/return coverage is 19923, final screenshot
inspected. Sky atlas capacity stayed at 128 MiB instead of the previous 256 MiB;
whole-GPU peak was 7764 MiB. Frame mean/p95/max: 19.66/20.78/45.32,
19.02/20.29/70.66, 11.30/21.80/118.64, 18.80/20.90/24.63 ms.
This addresses orphan lighting residency, not modern-world logical object
identity: the read-only ID probe still observes removal while away.

### Next bounded packed-texture experiment (OFF)

WGR_OBJECT_STREAM_PBO_TEXTURES=1 captures at most 64 immutable uncompressed PBO
texture spans per conversion on the owner thread, using IR stages and existing
adapter texture tables. Workers reuse ReadPAABlockChainBuffer and the existing
PreparedTextureStore/generation contract. Each source and decoded block chain
is capped at 16 MiB; prepared mip metadata is capped at 32 levels. Conversion
reservations include a conservative source/vector growth allowance, explicitly
not an allocator-capacity measurement. The store retains its own byte bound.
No worker calls the bank/MRU or uploads to the renderer. Other formats, stages
resolved only later, failed reads and refused publication retain the ordinary
path. The model PBO prototype now also respects enabled model DDC by declining
its bypass; it must not silently discard warm-cache performance.


### e0b88e7a packed texture runtime (experimental)

265 assertions / 20 PAA, preparation budget and cancellation tests passed.
`dayz-packed-textures-20260920-125624` completed the controlled route cleanly.
1375 prepared chains were consumed; main-thread texture-read time accumulated
5421 ms by route end. Do not compare that total with the preceding 486d5e9b
run's full-log total: that run includes a further 1500 m away/return.
First-full admission from engine log start took 34.3 s versus 31.7 s in that
preceding run; this is not yet a loading-time gain. Frame mean/p95/max:
19.90/22.10/71.36, 18.90/20.10/61.45, 20.21/24.13/56.15,
19.17/21.11/25.63 ms; whole-GPU peak 7720 MiB. Packed paths remain OFF.

The first experiment charged scratch against every queued/ready conversion,
producing a 5.58 GB reservation estimate even though only worker-count scratch
buffers can exist. Scratch is now charged only while the worker is active and
released before the Converted publication; model/IR reservations keep their
existing lifetime, including cancellation across Reset. Repeat validation pending.


### 08bbada3 active scratch, normal defaults, headless diagnosis

The active-worker scratch correction passed 265 assertions / 20 targeted tests.
`dayz-active-texture-scratch-20260920-130525` passed at factor 1.5 with both
packed prototypes enabled: conversion reservation peak 393,735,268 bytes,
final reservations zero, coverage 19923. Controlled-prefix texture read cost
5176 ms versus 12158 ms in the matched 486d5e9b prefix (excluding its extra
1500 m route). First full admission 28.7 s versus 31.7 s; one observation,
not a universal gain. Frame mean/p95/max: 19.05/20.41/86.81,
18.90/20.12/93.26, 19.13/20.20/54.45, 18.78/21.49/23.18 ms.
Whole-GPU peak 7404 MiB. Both packed prototypes remain OFF.

`dayz-default-settings-20260920-130807` passed with normal factor 2 and both
packed prototypes OFF, coverage 19923 before/return. Frame mean/p95/max:
19.80/21.08/33.46, 19.69/21.17/74.12, 19.58/20.80/38.76,
19.69/21.35/23.07 ms; whole-GPU peak 7776 MiB. This is a successful normal
configuration route, not proof that every former OOM is eliminated.

Private loopback headless legacy run `headless-legacy-20260920-131039` logged
simulation time progression, living AI and retained damage 0.25 without a
renderer. These are observations: the original runner discarded assertion
returns. Modern run `headless-dayz-20260920-131418` logged those simulation
values but FAILED the authored placement lookup: ID 83903272, known present
with geometry/fire geometry on the client, was absent. A clean process exit
is NOT a pass for modern-world gameplay correctness.

### Required logical identities: implementation under validation

Explicit script `object ID`, static network identity creation, save object
creation and reference restoration now use an owner-thread resolver. The
compact authored table is searched on a cold explicit lookup; repeat queries
use the existing ID cache. This may synchronously load one model; it does not
instantiate the whole map or manufacture a geometry-free object. The read-only
harness and FindObject/GetObject do not trigger admission or pinning.

Queried identities and objects with MustBeSaved state remain in the logical
object grid when the camera departs. Only visual registration and marker-light
ownership are retired, and the existing object is re-registered on return.
Sparse required/logical-only sets are cleared with the world and exposed in
residency counters; required CPU state is not claimed as reclaimed memory.
References remain retained for the world lifetime. Native far proxies stay
conservatively suppressed for retained objects, rather than drawing an
unmutated substitute for changed state.

This does NOT yet establish camera-independent actor-neighbourhood collision,
projectile corridors, navigation or nearest-object queries for never-requested
placements. Those still require a simulation demand policy. Aggressive visual
object-pressure control remains gated. Required-identity runtime verification
is recorded below; never-requested placement demand remains open.


### cd3d5c68 required identity runtime and d8ac1c8d capacity accounting

`dayz-logical-identity-20260920-133200` completed normal factor-2 traversal and
1500 m away/return. Explicitly requested ID 83903272 remained present with
geometry/fire-geometry payloads while away (required=1, logicalOnly=1).
The client evaluated unchanged object identity and damage 0.25 as true after
return. That is payload/state evidence, not yet an actual obstruction ray test.
Initial/controlled-return visual coverage 19923. Endpoint inspected; some tree
silhouettes differ from the prior endpoint, so numerical coverage is not a
blanket visual-equivalence claim. No renderer errors. Whole-GPU peak 7666 MiB.
Frame mean/p95/max: 23.28/30.28/50.12, 19.76/20.69/145.80,
13.36/19.83/125.33, 19.40/22.57/63.30 ms. This is mixed performance, with a
slower first pose versus the previous control; no global performance pass.

The initial headless runner discarded triAssertEq's returned string. Its
successful exit did not validate those assertions (presence/time/damage values
explicitly logged remain observations). The checked-in Test-HeadlessResidency
runner now logs and requires exactly three legacy or six DayZ OK replies,
and rejects FAIL replies. Strict run `headless-dayz-20260920-133810` logged
all six OKs: advancing simulation, living/damaged AI, existing authored object,
repeated identity and retained damage. This supersedes the loose initial checks.

PAA store admission/removal now charges vector capacity, including mip metadata,
while cumulative transfer counters still count useful payload. Map/string and
allocator overhead are explicitly excluded, not claimed as measured. A 1 MiB
test budget passed 280 assertions / 22 cases, including tiny valid payloads
whose retained capacities exceed the budget and exact release after Take/Upload.
`dayz-texture-capacity-20260920-133927` passed with both packed prototypes ON,
factor 1.5, visual coverage 19923 and final reservations zero. Peak conversion
reservation 318,598,236 bytes. Frame mean/p95/max: 20.19/22.47/93.20,
19.97/22.05/86.07, 12.98/23.30/145.61, 18.86/21.80/36.44 ms; whole-GPU
peak 7801 MiB. Packed prototypes remain OFF. This run validates accounting
integration, not a broad frame-time improvement.

The next diagnostics add read-only, bounded fire-geometry segment queries and
model names/positions to the identity probe. Their results follow.

### a92bab54/f4ffc510 actual obstruction and visual retirement

The bounded read-only fire probe does not resolve or pin objects. The first
fixture run `dayz-required-obstruction-20260920-134853` on a92bab54 FAILED its
fixture precondition: crosswalk ID 83903272 had fire geometry metadata but none
of the three chosen rays hit it. This is an invalid obstruction fixture, not
proof of a collision regression. No result.json was produced; failed artifacts
remain checksummed rather than relabelled successful.

f4ffc510 adds a visual-residency flag to the existing Object boolean storage.
The ordinary drawing selection rejects logical-only objects before CPU fallback
can draw or upload them; visual registration restores the flag on admission.
The fixture selector now requires a real fire contact before resolving/mutating
its identity, instead of treating fire-LOD presence as collision evidence.

`dayz-fire-identity-20260920-135655` PASSED using the installed f4ffc510 pair.
It selected ID 83912800, `platform1_stairs_30_wallr.p3d`, at
(5711.0610, 2512.3914, 68.4319), radius 4.7318 m. The 12 m horizontal fire ray
hit that identity before departure and again while the camera was 1500 m away.
The object remained present, visualResident changed true -> false -> true,
and returned identity/damage 0.25 evaluated true. Coverage returned to 19923;
shutdown completed cleanly with exit 0 and no renderer fatal error. This proves
one explicitly required obstruction survives visual retirement, not general
collision/query admission for never-requested objects or full save/network
round trips. The uncontrolled traversal is not a settled FPS comparison.

The stamp includes +dirty: pending changes were Cargo.lock generated by the
build and test-script capture/provenance changes; runtime C++/Rust sources were
committed f4ffc510. Provenance records the actual matching executable/DLL hashes
and the harness script SHA256. Images, logs and result are checksummed in the
manifest. Initial/returned images differed in vegetation silhouettes, requiring
a comparable-detail follow-up rather than accepting object count as equivalence.

### Fixed-detail visual follow-up — 26 September 2026

`dayz-fixed-detail-return-20260926-115132` PASSED on the same installed f4ffc510
binary hashes. The harness CoverageOnly control fixes the existing LOD governor
range at 1; it does not alter production defaults. Initial and returned-settled
(after eight seconds) images were inspected and show equivalent vegetation at
the test pose. Coverage restored to 19923; read-only selected identities remain
absent while away and return afterwards, as expected for unrequired objects.
Clean shutdown completed with exit 0. This supports LOD-governor/detail variation
as the explanation for the earlier capture difference at this pose; it does not
establish universal visual correctness. The diagnostic fixes detail explicitly
and supplies no normal-settings FPS/no-regression acceptance. Adaptive detail,
secondary views and mixed long-tail performance gates remain OPEN.
### Existing-build controls and same-world save/reload — 26 September 2026

`native-pre-deferral-control-20260926-120448` PASSED on the installed f4ffc510
pair before attachment deferral. The controlled normal-budget route restored
19999 placements, with zero final preparation reservations and clean shutdown.
Four settled frame mean/p95/max samples in ms were 30.22/32.09/34.32,
19.84/27.69/62.23, 29.92/33.96/90.40 and 21.22/24.92/27.33. The startup build
phase took 5121 ms: main meshes 0/1248, attachment meshes 525/525, attachment
objects placed 0. This is the candidate's pre-change workload control, not a
performance acceptance for the new code or Reforger asset correctness.

`dayz-save-away-roundtrip-20260926-115952` FAILED in the harness: triSaveGame
success returned the SQF string representation `"OK"`, while the runner expected
unquoted OK. The save file exists and is checksummed, but the interrupted test
did not establish a successful round trip. This is a strict reply-matching
error, not evidence that the engine save failed. The corrected runner accepts
the exact serialized string reply and still requires successful commands and
state/fire-query assertions.

Corrective run `dayz-save-away-control-20260926-120807` PASSED on the same
installed f4ffc510 executable/DLL hashes. Required ID 83912800 was saved at
damage 0.25 while visualResident was false and the camera was away. Damage was
then changed to 0.75 and checked before loading the saved full-game state.
Reload restored damage 0.25, authored identity and its fire-ray contact while
away; after return, visual residency and the script reference comparison were
also valid. Numerical coverage before/return was 19923 placements, and shutdown
completed cleanly with exit 0. The .fps save artifact, harness, provenance, result,
logs and available captures are checksummed. This is one same-world client
full-save round trip; fresh-process load, broader mutable state, headless save
and multiplayer/network validation remain open. The uncontrolled save route is
not a normal frame-time comparison.

### Committed increments awaiting candidate runtime validation

- d7c35c0f reserves the complete compact authored placement ID range before
  dynamic mission allocation and during save/load, cache rebuild and ID reset.
  Existing larger counters remain preserved. It also adds a bounded pure
  simulation-corridor planner with explicit Invalid/CapacityExceeded outcomes;
  no objects are loaded and no collision clearance is established by that planner.
  Actor/query admission ownership is still unimplemented.
- f0572b3c replaces the compressed-texture RGBA classification temporary with
  a compact exact-value alpha plane using the existing decoder contract. This
  targets classification scratch/conversion work, without reducing texture
  resolution or changing alpha decisions by intention. Candidate tests and
  runtime equivalence/performance acceptance are pending.
- 52b00a22 skips native attachment Shape construction at startup when streaming
  is enabled, matching the already deferred main-model path. All socket/prefab
  metadata, model paths and flattened part placements remain present; normal
  cold admission loads the shape/materials. The nonstreamed eager path remains
  intact. This removes startup work but may shift cold costs into admission;
  matched startup, settled/tail timing and coverage validation are pending.

These commits are implementation increments, not new runtime acceptance. The
verification build remains f4ffc510 until the candidate completes its gates.
### 11c5ca04 DayZ lifecycle validation and mixed timing

The complete supported build passed, followed by 6277 assertions in 92 targeted
cases. The first single-texel alpha reference used an invalid 1x1 PAA fixture:
the reference reader requires the format's minimum supported dimensions.
b12e46ca supplies a valid reference fixture; that failed fixture is not a runtime
alpha-rendering failure. Compressed alpha-plane parity preserves exact decoded
alpha/class decisions against the canonical decoder in these tests; no texture
resolution change or general FPS improvement follows from that result.

Same-day old-build control `dayz-pre-alpha-control-20260926-121957` and candidate
`dayz-compact-alpha-candidate-20260926-122958` both PASSED their controlled routes,
settled idle/unchanged-demand checks and clean exit. Old control provenance lists
checkout HEAD 52b00a22, but its installed stamp and executable/DLL hashes identify
actual runtime f4ffc510. The candidate is the clean installed 11c5ca04 pair,
including source increments through 52b00a22 and the valid alpha fixture. The
native attachment gate does not execute for this DayZ corpus. Later uncommitted
work is not in either measured binary.

Both routes admitted/restored 19923 at the base pose. Controlled per-pose resident
counts were identical: 19972 / 19966 / 19972 / 19923. Frame mean/p95/max in ms:

| Pose | f4ffc510 control | 11c5ca04 candidate |
|---|---|---|
| +200 m | 21.04 / 24.95 / 52.52 | 22.67 / 27.40 / 51.42 |
| +400 m | 21.14 / 29.76 / 191.79 | 26.45 / 44.70 / 132.86 |
| +200 m return | 15.70 / 24.04 / 125.69 | 8.99 / 12.83 / 99.00 |
| Base return | 18.93 / 23.50 / 28.76 | 19.15 / 28.05 / 144.21 |

Through STREAM_TRAVERSE_END, cold-profile rows total 736/731 (control/candidate).
Their accumulated texture read/create/alpha costs are respectively
13068.1/1409.5/3120.0 ms and 14371.0/1552.0/3494.3 ms. First full admission,
measured from the first engine log line, is 35.304/38.216 seconds. These numbers
do not show an overall loading or alpha-time gain. Tests were ordered control
then candidate, making file-cache/driver and warm-path effects confounded.

Actual render demand also differs despite matching placement counts. Inside
pose measurement markers, main triangles are control/candidate: pose 0
1.287–2.019 M / 1.044–1.209 M; pose 1 0.616 M / 0.405–0.442 M;
pose 2 0.965 M / 0.546 M; pose 3 0.620–1.403 M / 0.382 M. LOD histograms
confirm differing selected detail. For example, pose 2's control histogram
is 412/1198/1668/1499/439/68/6/5 (LOD0..7+), whereas the candidate is
82/696/906/1016/321/60/4/0. The active governor changes detail within some
measurement windows. Faster candidate pose 2 therefore cannot be advertised as
an equal-quality FPS gain; worse candidate p95/maxima also prevent a blanket
no-regression acceptance. Counts alone are not visual equivalence.

Acceptance here is lifecycle/build/decoder parity only. Matched fixed-detail
performance/visual evidence, candidate native attachment admission and the new
fresh-process save test remain pending. Existing off-by-default pressure,
packed-preparation and adaptive-detail gates stay unchanged. No source change
is promoted as a universal performance fix by this comparison.
### Fresh-process saved-object restoration on 11c5ca04

`dayz-fresh-save-candidate-20260926-123431` PASSED with the clean installed
11c5ca04 pair. The first client saved required obstruction ID 83912800 at damage
0.25 while the camera was away and its visual residency was retired, then
changed damage to 0.75 and verified that control. The owning harness exited that
client cleanly and started a fresh game process with the same private profile.
Before triLoadGame, the read-only identity probe reported the fixture absent;
it did not pin or resolve the object. Loading the full save restored the authored
identity, damage 0.25 and its real fire-ray contact while the camera was still
away. On camera return visualResident became true and the saved script reference
comparison remained true. Both processes exited cleanly; initial and reload logs
contain no renderer fatal errors. Base numerical coverage returned to 19923.

Both process logs, common harness/provenance, result, available images and the
.fps save artifact are checksummed. This closes the narrower fresh-process
client restoration gate for this explicitly required, mutated static obstacle.
It does not validate every mutable component or multiplayer/network/headless
save behavior, nor admit never-requested actor-neighbourhood/query geometry.
This is an uncontrolled correctness route and provides no performance acceptance.
Candidate native attachment streaming and equal-detail performance/visual gates
remain pending independently.
### Native attachment deferral: startup/lifecycle passed, performance gate open

`native-deferred-attachments-candidate-20260926-123842` PASSED on 11c5ca04 with
clean shutdown, unchanged settled demand and 19999 restored placements. The
same-day f4 control is `native-pre-deferral-control-20260926-120448`. Both retain
1296350 compact placements, 1773 model paths, 330 slot-bearing prefabs, 8836 socket
bones, 525 distinct part meshes and 3986 buildings with attachments; no socket
resolution failures are reported. The change preserves path/index generation
and all flattened attachment placements by construction, instead of suppressing
parts to reduce work. Both endpoint screenshots were inspected: the street,
buildings, windows, fences and signs remain present with comparable local
coverage. This is a same-scene change check, not ground-truth Reforger material,
furniture or asset-completeness validation.

Eager attachment shapes fell from 525 to zero and the startup shape-build phase
from 5121 to 0 ms. First full admission, from the first engine log line, fell from
54.752 to 50.503 seconds. Part work moves into the ordinary admission pipeline:
at first full admission the cold-model count rises from 503 to 755, aggregate
worker time 30726 -> 39883 ms, maximum worker job 1632 -> 2219 ms, and owner-thread
cold optimization totals 8139 -> 10991 ms. These are cumulative profiling sums,
not wall-clock waits or exact allocator measurements. Peak conversion reservation
rises 173786868 -> 195817052 bytes, while peak ready payload falls
194226893 -> 129502661 bytes; final reservations are zero.

Peak backend allocation observations are 2995051136 / 2990779680 bytes
(control/candidate). Final geometry capacity stays 704643072 bytes; live geometry
is 624496320 / 523627400 bytes and final backend allocations 2970787616 /
2970112128 bytes. Capacity is retained, and neither counters nor fewer live bytes
establish complete equivalent GPU coverage or exact driver VRAM savings.

| Pose | f4ffc510 control mean/p95/max ms | 11c5ca04 candidate mean/p95/max ms |
|---|---|---|
| +200 m | 30.22 / 32.09 / 34.32 | 57.66 / 68.99 / 71.80 |
| +400 m | 19.84 / 27.69 / 62.23 | 20.25 / 26.90 / 59.55 |
| +200 m return | 29.92 / 33.96 / 90.40 | 31.66 / 34.79 / 42.98 |
| Base return | 21.22 / 24.92 / 27.33 | 23.30 / 27.82 / 49.01 |

Per-pose placement counts match: 19997 / 19996 / 19997 / 19999. Main render
triangles still differ: control/candidate pose 0 4.705–5.049 M / 3.866 M,
pose 1 5.219–5.942 M / 5.617–6.708 M, pose 2 4.501–4.505 M /
4.006–5.144 M, and pose 3 6.671–7.008 M / 6.520–7.666 M. LOD histograms confirm
varying governor selection; equal placement counts are not equal rendering work.

The first candidate pose is a material slowdown signal even with fewer main-view
triangles: swap averages 54.28 ms versus 28.00 ms. GPU samples also show higher
shadow time (about 15.9–16.9 versus 7.5–8.5 ms), terrain colour and AO costs,
while cull cost stays similar. No matching admission activity is recorded within
that settled sample. This does not establish that startup deferral caused the
GPU slowdown, but it prevents accepting the change as regression-free. Candidate
also combines ID reservation and compact alpha work, and ordered runtime/cache,
LOD and possible external GPU effects are uncontrolled. Startup deferral is
verified; matched fixed-detail GPU/performance attribution remains an open gate.
First-pose attribution follow-up: candidate retained mesh, texture, geometry and
backend allocation counters stay fixed throughout the measurement markers. No
cold admission, geometry growth, texture upload or far-tier event is logged in
that window. The last geometry growth finished at 12:39:32, about 34 seconds
before the sample; sky-atlas growth was at 12:39:11. The next reported sky-bake
gate change is after the first pose. Native authored-far demand is opt-in and no
far-tier activity is reported. Existing telemetry cannot rule out every hidden
GPU task, but does not support attributing the slowdown to ongoing admission.

Grass census is identical at 15992 near clumps, 90 mid cards and 6717360 grass
triangles/frame. Candidate shadow triangles are also lower, yet water draw costs
11.8–14.2 ms versus about 6.2 ms, and colour-pass costs 28.6–32.1 versus
14.5–15.7 ms. Broad raster/fragment stages become slower while cull is stable.
The next useful diagnostic is a persistent fixed-detail first pose with stable
resource counters, water node/triangle statistics, grass fragment counts and
GPU stage samples, including a base/return leg to test whether the state recurs.
External GPU load/clocks and missing hidden-work telemetry remain uncertainties;
CPU alpha classification is not established as the cause.

The attachment deferral prototype is now gated in source by
WGR_NATIVE_DEFER_ATTACHMENT_SHAPES=1, and only when native streaming is enabled.
Unset/0 keeps the old eager attachment startup as default; nonstreamed placement
remains eager. The main-model deferral is unchanged. The measured 11c5ca04 build
had the automatic candidate behavior and proves startup/lifecycle only. The
new default-restoration guard still needs its committed build/runtime check;
it is not retroactively included in the measured candidate. Deferral remains
OFF pending the performance attribution gate.
### Isolated alpha CPU benchmark and next committed preparation foundations

The standalone diagnostic under `build/alpha-microbenchmark` compares separately
compiled production-source baseline/candidate paths with /O2, no LTO, fresh
scratch allocations inside timing, alternating arm order and 18 BC2/BC3 cases:
512/1024/2048 textures, opaque/mask/blend patterns. All 18 parity checks passed.
The first run uses 15 batches x 2 repetitions; longer confirmation uses
31 batches x 3. Confirmation candidate/baseline median ratios are
0.3909–0.6831 for BC2 and 0.6238–0.9455 for BC3. At 2048 dimensions the scratch
vector's logical size falls from 16777216 to 4194304 bytes (16 -> 4 MiB).
This is isolated algorithmic CPU evidence; allocator capacity, GPU/game FPS,
map loading and the pre-existing RGBA fixed-stride wrapper are not established
by the benchmark. Some short-run p95 values are noisier; no universal latency
or engine performance claim is made. Actual source/provenance, compiler script,
empty successful compile log, binary and both CSV artifacts are checksummed.

91a2073e deduplicates successfully completed optional material-input reads within
one model preparation job. It retains embedded/face texture inputs, preserves
available partial material results and retries failed/incomplete inheritance;
no cross-job mutable cache is introduced. Its conservative simulation-source
bounds foundation marks only CompleteSource or Unknown. CompleteSource is not
engine collision readiness: adaptation, synthesized geometry, animation/proxies
and placement transforms require separate validation before admission can use
any bound. It is not yet integrated into runtime actor/query loading.

b4bdb20b commits the native attachment prototype's opt-in guard and restores eager
attachment startup by default. The optional whole-device sampler now includes
GPU/memory utilization, graphics/memory clocks, power, temperature and pstate
at one-second intervals. These include other applications and are diagnostics,
not per-game resource attribution. The guard/new material preparation/bounds
changes are not in the measured 11c5ca04 binary; their new build/runtime checks
remain pending. The authoritative streaming plan stays OPEN.