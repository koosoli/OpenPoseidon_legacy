# Streaming execution audit — 20 September 2026

Baseline: `03e713714b9b699e3f6f4eae7cdce7f488a13b64`, previously

`codex/ocean-release`, now `codex/streaming-residency`. The dedicated worktree is

`.claude/worktrees/combat-audit`; other checkouts are not integration targets.

The wave prototype is parked on GitHub remote `safety`, branch

`codex/backup-waves-20260920`. The old ocean remains default.

## Verified ownership and routing
| Area | Current owner and result |
|---|---|
| Residency invocation | `World.cpp`, before `CapturePresentationSnapshot`, inside `enableDraw` and renderer capability/camera checks. Already moved from TerrainWgpu. Separate default-OFF simulation residency now pumps at the owner boundary after background AI joins; existing camera residency remains independent. |
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
  Cold CPU-only explicit wall resolution and later visual promotion now pass;
  one cold-destroyed wall also preserves identity/phase and defers visual buffers.
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
  Bounded allocation-only diagnostic now passes partial-subset attribution; no selected-LOD usage or paging acceptance. No hierarchical page implementation is promoted on this evidence.
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
### 10f6e695 normal native default and material-preparation integration

The supported build passed and 6443 assertions in 103 targeted cases passed.
`native-default-material-dedup-20260926-130401` then PASSED its normal controlled
route on the clean installed 10f6e695 pair. Startup explicitly logs attachment
shape deferral disabled and builds 525 attachment shapes. The restored default
therefore runs the old eager attachment path; the optional prototype stays OFF.
Numerical per-pose coverage is 19997 / 19996 / 19997 / 19999, with 19999 restored,
zero final reservations, no renderer fatal errors and clean exit.

| Pose | 10f6e695 frame mean/p95/max ms |
|---|---|
| +200 m | 31.40 / 34.72 / 93.88 |
| +400 m | 24.06 / 25.85 / 27.05 |
| +200 m return | 31.66 / 36.23 / 42.78 |
| Base return | 21.23 / 25.19 / 65.97 |

First-pose raster costs return toward the old f4 control range: shadows around
6.95–8.77 ms, water draw around 6.86 ms and swap mean 28.84 ms, compared with
11c5ca04's automatic-deferral candidate at roughly 16 ms shadows, 12 ms water
and 54.28 ms swap. This does not isolate the cause: source/settings differ,
LOD selection still changes during samples, and there are no matching old-arm
clock samples. Current main triangles span 4.006–4.771 M at pose 0,
6.912 M at pose 1, 4.006–5.107 M at pose 2 and 6.903 M at pose 3. Consequently
this successful default route is not blanket equal-quality/no-regression evidence.

Loading/preparation remains mixed and is slower in this observation. The eager
shape phase is 8780 ms versus 5121 ms in the old f4 control. First full admission
from the first engine log line is 86.320 seconds versus 54.752 f4 and 50.503 for
the deferred 11c5ca04 candidate. First-full cold model count is 503, matching f4
(and 755 with deferral); aggregate worker time is 46661 ms/max job 2555 ms versus
f4 30726/max 1632. Peak conversion/ready reservations are 207200834/203526363
bytes. Through STREAM_TRAVERSE_END, cold-profile rows total 864 versus f4 867;
accumulated read/create/alpha costs are 201.3/4293.9/2.3 ms versus
111.7/2904.2/1.3 ms. Optional texture-input preparation worker time sums 632 ms
versus 807 ms, with zero prepared chains consumed in both and the same consumed
model counter 1018. These are cumulative profiling observations, not complete
material-read counts or a proven dedup loading-time gain. Cache state, combined
increments and system scheduling remain uncontrolled.

The added one-second whole-device sampler peaked at 4449 MiB. During pose 0 it
reports P0, graphics clock 1920 MHz, memory clock 7001 MHz, utilization 99%, power
about 210–212 W and temperature 71–73 C. Its error log is empty. Older controls
lack comparable clock/power instrumentation, and these whole-device samples
include other applications; they cannot retrospectively explain the 11c slowdown.
Reported backend allocation peak is 2994881536 bytes, final geometry live/capacity
624496320/704643072 bytes, final backend allocations 2970803888 bytes.

Material-input dedup is now integrated through this native runtime route; no
loading/performance improvement is claimed. Simulation bounds remain only
CompleteSource/Unknown metadata foundations. The next required gameplay gate
is bounded owner-thread logical admission for never-requested actor/query
geometry, with cold CPU collision verification; that runtime admission is still
pending. Multiplayer, broader mutable state and equivalent-detail performance
gates remain OPEN.
### 037f188d cold CPU-only logical residency and deferred texture window

Production commit `68143711` adds CPU-only cold logical shape loading and later
visual/proxy promotion; `037f188d` moves fallback BC3 encoding before renderer
ownership, using the limited window for the first eligible work item per frame.
The supported combined build passed, and the targeted test log records **6565
assertions in 110 cases**. Those checks cover the compiled source, including
logical proxy promotion and texture-window ownership guards. They establish no
loading/FPS improvement or broad no-regression acceptance.

The installed executable/renderer pair is clean-stamped
`codex/streaming-residency 037f188d 2026-09-26_13:30 deploy-ps1`;
binary and per-run provenance hashes are in the manifest. The two cold-logical
attempts below are deliberately **not PASS**:

- `dayz-cold-logical-20260926-133048`: the read-only identity probe initially
  finds fixture 83912800 absent. Explicit script resolution then produces the
  real platform/stairs/wall identity, `visualResident=false`,
  `shapeVisualDeferred=true`, physical/fire geometry and an actual fire-ray
  contact. However, the global geometry counter increases from 67037524 to
  67147508 bytes (+109984) between the initial `pending=false` snapshot and the
  post-resolve sample. The initial snapshot preceded the final visual drain;
  the equality assertion therefore has no stable baseline. These counters
  cannot attribute the delta to the cold logical object. The runner does not
  complete visual-promotion acceptance and produces no result artifact.
- `dayz-cold-logical-20260926-133403`: runner commit `1f04f324` first waits for
  repeated stable GPU geometry samples. Samples remain at 67147508 bytes,
  although texture/backend allocations still change. The engine then logs a
  harness socket disconnect and clean shutdown with exit code 0 before the
  identity probe. Later launcher/source analysis identifies the sea-origin scenario setup
  as the explanation (below). It proves neither cold resolution nor promotion, and there is no
  result artifact. Clean process exit alone is not test acceptance.

Both harness and engine logs and both provenance records are hashed as actually
present. No missing result file or screenshot is invented. The revised runner
addresses the baseline timing defect; the remaining installed cold-identity and
visual-promotion acceptance gate stays OPEN.

A separate `SimulationCoverageIndex.hpp` foundation and six test cases are now
compiled and tested in the subsequent bd9b472d combined build (below), but remain
**unused metadata foundations**, with no automatic runtime demand integration.
It accepts numeric evidence from a validated fully adapted engine shape and
final repaired position/scale, enlarging the existing engine guard sphere by
2x and conservative roundoff. Uncertified model groups block coverage globally;
certified disjoint group envelopes can allow progress. Monotonic generation
leases, cancellation and bounded scans/matches reject stale or partial answers.
`Ready` means metadata coverage only, never collision installed or obstacle-free.
The producer/publisher still must certify every placement in a group and
invalidate records after content/frame changes. No automatic actor/query/AI
admission is wired, and the overall streaming plan remains OPEN.
### Compiled coverage foundation and logical destruction-buffer deferral

`cba650fd` commits the bounded simulation coverage metadata foundation, and
`bd9b472d` defers destroyed logical-object buffer work until visual admission.
The combined supported build passed; the targeted log records **6685 assertions
in 120 cases**, including coverage and logical shape checks. The new build/test
logs are hashed separately in the evidence manifest. The clean installed build
is stamped `codex/streaming-residency 9c35f49d 2026-09-26_13:44 deploy-ps1`.

The coverage foundation is now compiled/tested but unused by automatic runtime
query, actor or AI demand. Its metadata Ready status still does not mean installed
collision or clearance. Logical destruction-buffer deferral has source/unit
validation; its current installed runtime verification is pending. No running
smoke test is classified as completed evidence here. The two earlier incomplete
cold-logical attempts retain their classifications, and performance, automatic
simulation demand, broader state and multiplayer gates remain OPEN.
### Strict headless identity pass and corrected cold-logical scenario origin

`headless-dayz-20260926-134511` PASSED on the matching 9c35f49d server build:
engine/console logs contain six explicit `STREAM_ASSERT_OK` lines, followed by
`triEndTest` and clean exit 0. The mission verifies simulation time, alive/damage
state of the mission probe and explicit placement 83903272 existence, repeated
identity and damage .25. This is headless identity/damage acceptance only;
it does not test fire geometry, full save, multiplayer or automatic demand.
Both logs, provenance and the actual mission/init sources are hashed.

The third graphic attempt is actually
`dayz-cold-logical-20260926-134647`. It reaches a stable geometry baseline, then
ends before the cold identity probe, so remains INCOMPLETE rather than PASS.
Root diagnosed the repeated clean early exits as a scenario setup issue:
`apps/cwr/Game/GameApplication.cpp:2075–2090` grounds the freefly camera carrier
at terrain height for the requested x/z. The launcher log records player
position `100, -49.12667, 100`, below sea at the runner's sea origin. The source
explains the drowning/end-mission route to clean exit. This source-supported
scenario diagnosis supersedes the earlier unspecified closure classification;
it is not evidence of a crash or cold-logical engine regression.

Runner-only commit `9fa064f7` changes the initial freefly pose to inland
`10000, 10000, 450`; it makes no engine code change. The corrected attempt later passes the explicit identity gate below; it does
not turn the interrupted attempts into passes. The first unstable-counter attempt
and both pre-probe interruptions retain their incomplete classifications.
### Cold explicit logical identity and later visual promotion PASS

`dayz-cold-logical-20260926-134949` PASSED on clean installed 9c35f49d with
runner commit 9fa064f7's inland origin. The initial read-only probe confirms
real wall fixture **83912800** absent; explicit script resolution then supplies
physical/fire geometry and a real fire-ray contact, while `visualResident=false`
and `shapeVisualDeferred=true`. After a stable baseline, global geometry live
bytes remain **35207752 before and after cold resolution**; capacity, texture
and backend snapshot counters are also unchanged. This supports CPU-only cold
creation for this fixture without attributing unrelated pending visual work.
It does not measure CPU allocations or prove all models avoid GPU work.

The cold phase deliberately uses a 1000-object diagnostic budget (921 resident
placements). Returning to the fixture with a 20000 budget converges to **19923**
placements. The same fixture becomes `visualResident=true` and
`shapeVisualDeferred=false`; script reference equality and damage .25 remain
true, and the known wall still appears in the fire contacts. Root inspected
`promoted.png` and saw the road, buildings and vegetation. The runner writes
`passed=true`, requests exit itself, and the engine exits cleanly with no fatal
renderer error. Result, harness/engine logs, provenance and promoted image are
hashed in the evidence manifest.

This closes the **single explicit cold logical identity/fire/promotion** runtime
gate. It does not close automatic query/actor/AI admission, general collision
completeness, runtime destruction-state coverage, wider save/multiplayer state,
secondary views, or equal-quality performance. The small cold budget is a
correctness diagnostic, not a normal-settings performance improvement. The
metadata coverage index is compiled/tested but remains runtime-unused, and the
streaming plan stays OPEN. The three earlier graphic attempts remain incomplete.
### Model-space certificates, exact placement repair and destroyed cold wall PASS

Production `4fcfeea8` publishes persistent model-radius metadata, shares the
exact `ObjectCreate` orientation repair and exposes CPU encode-before-window
instrumentation. The supported build passed; targeted tests report **6882
assertions in 131 cases**. Both actual build/test logs are hashed. No FPS or
loading gain is inferred from these source/ownership checks or counters.

Radius tokens retain generation and exact owned model identity, survive payload
Take/DropStale and reject stale inventories/conversions. MLOD/XOB source bounds
remain Unknown until adaptation; tested canonical ODOL/adapted scalars can become
Certified. `radiusCertificateBytes` charges RadiusSlot vector allocation capacity
(fresh on Reset), not all preparer/string/allocator storage. The shared placement
helper preserves existing float RMS scale, forest bypass, Network scale 1 and
ClipLandKeep repair; legacy elevation correction remains at its original site.

**Constructor reconciliation remains a correctness gate.** Worker QueryRadius
certifies only pre-constructor model-space evidence. Config-backed NewObject can
replace the model with a Forest/type shape, and VehicleAddRef can alter shared
shape autocenter and recompute its radius. A world coverage producer still needs
actual post-constructor shape radius, actual final repaired position/scale and
validated eligibility or reconciliation. These tokens/helper cannot certify all
object types. Unknown coverage must never become Ready from guessed class/type,
raw transforms, source bounds or a 25 m padding. Automatic actor/query/AI admission
remains unintegrated.

`dayz-cold-logical-20260926-141048` is a FAILED HARNESS predicate, not an engine
destruction failure: setDammage 1.1 produces a destroyed object, but getDammage
clamps its returned total to 1 (`Object.hpp:668`). The runner wrongly expected
1.09–1.11. Logs already show cold wall 83912800 destroyed at phase 0.9960784912
with visual=false/deferred=true/normalVB=0, then visual=true/deferred=false/normalVB=4
and unchanged identity/phase on return. The false damage predicate prevents a
result artifact; no absent result/image is invented or retroactively marked PASS.
Runner-only `d8b7126f` separates the clamped getter check from destruction state.

The corrected `dayz-cold-logical-20260926-141854` PASSED with `destroyCold=true`
and `passed=true`, runner HEAD d8b7126f and clean installed d70fd977 (14:10 stamp).
Before destruction, the initially absent real wall gains CPU physical/fire
geometry and a real fire contact, while stable global geometry is unchanged at
35207752 bytes across cold resolution. After destruction its phase is
0.9960784912, visual=false/deferred=true and normalVB=0. Returning with budget 20000
converges to 19923 placements and the same wall/reference/phase, visual=true,
deferred=false and normalVB=4; the corrected clamped damage getter check is true.
The root inspected promoted.png and saw normal street assets. The runner requests
exit itself and shutdown completes cleanly at 14:19:57. Engine/harness/provenance,
actual result and promoted image are hashed.

This is one explicit cold destruction-state/buffer-promotion case, with an initial
1000-object diagnostic budget. The destroyed wall is not required to remain a
post-destruction fire blocker; the returned ray contains other scene contacts,
and no such wall-hit persistence is claimed. Broader destruction, full save/MP,
constructor eligibility, automatic simulation demand, secondary views and
comparable-quality performance remain OPEN. The streaming plan is not complete.
### Native fixed-LOD coverage and texture-window opportunity

`native-window-encode-20260926-142234` PASSED its targeted coverage scenario on
clean installed d70fd977, with normal 20000 object budget, factor 2, packed paths
OFF, adaptive/pressure growth OFF and fixed lodGovernorRange1. It restores 19999
placements, drains reservations and exits cleanly. Root inspected initial.png
and returned-settled.png: street/structures are present in both, but vegetation
appearance visibly differs. This is fixed-LOD diagnostic coverage, not normal
FPS measurement or exact vegetation-quality parity. Native Reforger remains a
stress workload with known compatibility anomalies, never asset ground truth.

Across all 1466 cold-model profile rows in this log, texture encoding sums
1458.605 ms. The explicit `texEncodeWithoutWindow` sum is 223.066 ms over 16 encodes.
This reports eligible CPU work completed before renderer acquisition; it does
not demonstrate concurrent GPU work, overlap, FPS gain or loading improvement.
The source currently opens this limited preparation window for the first
eligible item per frame, so the counter is not coverage of all fallback encodes.

A remaining Betula_Pendula/t_betula_pendula_3s.xob cold row reports
`texWindowWait=583.524`, `texEncodeMs=60.185` and
`texEncodeWithoutWindow=0.000/0`, with total cold cost 683.4 ms. This is a measured
renderer-window acquisition/preparation bottleneck worth targeting; the previous
ownership change has not eliminated it. The next correction must preserve
producer/render ownership and validate comparable content/detail rather than
crediting fewer objects or different vegetation as performance improvement.

Before/returned geometry counters match 642734708 live/704643072 capacity bytes;
radius certificate capacity is 14184 bytes, peak conversion/ready reservations
172255128/183109033 bytes. These counters are scoped as described above, not
allocator-exact or complete driver memory. The optional one-second whole-GPU
sampler includes other applications; its error file is empty. Engine/harness,
provenance/result, three actually present images and sampler files are hashed.
The streaming plan, texture stall attribution and visual/performance gates stay
OPEN.
### Live instance observations and bounded native DDS/BC3 prototype

`69f9b479` adds owner-thread live-instance observations, a local shape query-policy
revision and weak object/shape lifetime witnesses. The observation samples the
actual constructed object frame and radius, revalidates them within a lexical
owner-operation lease, and refuses stale movement, identity, mutation, wrong
owner or expired scope. Weak witnesses reject an identical-address replacement
without retaining the object/shape. This is evidence for **one existing live
instance**, not a model-group certificate, persistent world/config epoch or AI
publication barrier. It does not certify arbitrary per-LOD mutation/animation,
geometry installed or a clear query. ID-cache observations do not load or pin.

`dde3d4fb` extracts the exact legacy CPU BC3 block/mip encoder and optionally
prepares upload-only sidecars for eligible native DDS composites. Source and
workspace/output reservations are bounded; budget refusal keeps ordinary DDS
preparation and fallback progress. Source pixels/classification stay available;
owner choice is captured, and adoption checks options, identity/name and
source dimensions with a one-shot claim. Native DDS preparation remains default
OFF (`WGR_NATIVE_DDS_PREPARE=1` opts in). GPU/bank ownership stays with its
existing owner. Source/parity/budget-skip/parse-cancellation tests pass; cancelling
while actively encoding remains untested. Reservations are not allocator-exact.

The final supported build passed. Default-OFF regression checks passed **7330
assertions in 150 cases: 145 passed, 5 explicitly skipped** for enabled-only native
fixtures. A fresh ON process passed **292 assertions in 10 cases**, no skips.
Earlier initial/retry SRef-cast compile errors and private fixture BindPreparedMips
access were repaired before this build; they are not runtime regressions. Actual
successful and failed build/test logs are separately hashed. EngineTools also
parses the generated fixture as two authored placements and four MLOD levels;
those tool results/content hashes establish parsing, not simulation admission.

Both native coverage runs use the same clean installed dde3d4fb pair (15:08 stamp),
normal 20000 budget, factor 2 and fixed lodGovernorRange1. Both write passed=true,
restore 19999, drain reservations and exit cleanly. Root reviewed initial/returned
images: street/structures present, vegetation appearance varies again. Reforger
is workload evidence, not ground truth, and fixed-LOD coverage is not normal FPS.

| Measured scope | Native DDS/BC3 ON | Default OFF |
|---|---:|---:|
| First full admission from first engine log |49.924s|39.318s|
| First engine log to shutdown complete |107.590s|87.311s|
| Owner encode across 1466 cold rows |1146.570ms|1564.368ms|
| CPU encoding before window |306.974ms/13|303.199ms/17|
| CPU sidecar claims/bytes |30/21321024|0/0|
| Worker prepared/time |43/2226.5582ms|0/0|
| Peak encode reservation bytes |32856480|0|
| Peak conversion/ready bytes |133490202/132545020|206436020/191289445|

At equivalent allocated counters, both have geometry 642734708 live/704643072
capacity bytes and object textures 995821696 bytes/1298 items. Reported backend
allocations are 2964816304 ON versus 2963766560 OFF (earlier control 2964815152).
These counts do not establish exact visual parity or complete driver memory.
The OFF run verifies worker preparation/claims/encode reservations remain zero.

The ON Betula3s cold row drops to 11.4 ms, owner encoding 0 and two CPU claims totaling
2796192 bytes; the earlier control row was 683.4 ms, including 583.524 ms window wait
and 60.185 ms encoding. This is a useful local opportunity, **not a headline gain**:
ON includes native DDS composition as well as BC3 preparation, and whole first
admission/scenario latency is slower than the matched OFF run. Single sequential
runs have cache/system ordering confounds. CPU claims do not establish GPU
completion/overlap. The prototype stays OFF pending causal diagnosis and
performance/quality acceptance. Runtime logs/results/provenance, actual images
and whole-device sampler artifacts are hashed for both arms.

Automatic static actor residency is now a committed separate default-OFF prototype
with a passing two-wall runtime fixture below. Generic query consumers, AI and
navigation have not migrated, and generic clearance completeness remains open. Constructor/group
coverage, active-encode cancellation, secondary views, wider mutable state/MP and
comparable-quality performance remain OPEN; the streaming plan is incomplete.

### Default-OFF static actor leases — 26 September 2026

Source 766bef7b limits optional native DDS preparation to successful BC3
composites; both preparation and this narrower switch remain OFF by default.
The matched BC3-only performance comparison is recorded below; loading remains slower ON.
0afcac2c adds a separate owner simulation residency service, enabled only by
WGR_SIMULATION_RESIDENCY=1, with source 4 MiB, payload 64 MiB and Shape scheduling
256 MiB bounds. Scheduling charges are estimates, not allocator RSS. Inventory
is bounded at 256 model groups/one million placements; unsupported groups remain
globally Unknown. Eligibility currently covers static authored Geom/Fire/View
payloads, not arbitrary constructor classes, animated/proxy models or all queries.

The supported retry build passed after ten OLink GetRef compile errors were
repaired; the failed build log is retained separately. Default regression passed
7389 assertions/156 cases (150 passed,6 explicit skips); broad preparation
292/11 (10 passed,1 skip); narrow preparation 349/11 (9 passed,2 skips).
Enabled-only and mutually exclusive preparation fixture skips are explicit.

Both actor fixtures use the clean 15:47 installed 0afcac2c pair. Its embedded
build tag is 826a189a because compilation preceded the source commit; functional
code corresponds to 0afcac2c and actual binary hashes are recorded. Corrective
runner/generator a9548bf8 uses original private absolute-model files. The first
154800 attempt failed CLI validation because addon-root requires PBO archives;
it produced no harness/result and is not a collision regression.

Default OFF (155037) passes with Disabled, inventory/ownerPumps/created zero and
both walls absent. ON (155116) passes without ResolveObject or permanent pins:
two authored walls/one model/two Ready regions admit cold CPU geometry/fire with
visualResident=false, deferred=true and normalVB0; the fire segment hits1001.
Camera promotion renders both original white walls (root reviewed visual.png).
Moving the camera away preserves actor leases and fire contact. Mutating1002 to
damage .25 then moving the actor to100 releases all leases: unmodified1001 is
removed;1002 remains MustBeSaved, phase0, damage .25, required0. Both runs exit0.
The shared Shape retains one normal VB after visual promotion while logical-only;
this proves logical/state leases, not GPU eviction.

No existing AI, collision, navigation or generic query caller is migrated.
Actor-region Ready is not a certificate for arbitrary rays. Existing origin-grid
25m candidate enumeration can omit a large building whose origin lies farther
away; this approximately 6m fixture does not close that generic clearance gate.
Exact query demand/readiness now passes the separate narrow fixture below; generic consumers remain unmigrated. Broad
performance, active-encode cancellation, complete class coverage, secondary views,
wider mutable state and MP authority remain open. The streaming plan is OPEN.

### Narrow BC3-only matched pair — 26 September 2026

The candidate 155306 and default 155603 both pass the coverage route, exit0 and
restore 19999 on the same clean 0afcac2c installed pair, fixed LOD range 1, normal
20000 budget and whole-device sampler. Root reviewed both initial/returned
images: street, structures and trees closely match between arms; vegetation,
wind and lighting vary on return. This is not exact pixel parity, complete
Reforger compatibility or a normal FPS measurement.

| Measured scope | BC3-only ON | Default OFF |
|---|---:|---:|
| First full admission from first engine log |38.080s|36.626s|
| First engine log to shutdown |85.142s|81.851s|
| Residency phase elapsed |17.563/5.801/16.607s|16.776/5.769/16.463s|
| Owner encoding,1466 cold rows |39.407ms|1408.813ms|
| Renderer window wait, same cold rows |3843.298ms|2712.875ms|
| CPU encoding before window |32.458ms/1|277.828ms/16|
| CPU claims/bytes in cold profiles |74/60358688|0/0|

Both report identical geometry 642734708 live/704643072 capacity bytes, object
textures 995821696 bytes/1298 items and backend 2964815152. Candidate before
reports 104 prepared, 2188.0521 worker ms, 104 DDS puts/90 takes/0 configuration
misses,one remaining cache entry/109668 bytes; OFF preparation counters are zero.
Profile claims cover the cold rows, not every sidecar transfer or GPU completion.

The first Betula3s row is 99.5ms ON (window 57.240ms,owner encoding 0,two claims/
2796192 bytes) versus 639.0ms OFF (window 545.330ms,encoding 53.241ms,no claims).
Narrowing avoids broad unused DDS compositions/cache growth and removes much owner
encoding, but total first-full/scenario latency remains approximately 4% slower
ON. This single ordered pair has cache/system confounds; it neither establishes
a headline FPS improvement nor meets the performance gate. Both switches remain
default OFF; the complete streaming plan stays OPEN. Actual logs, results,
provenance, images, sampler files and the read-only analysis/output are hashed.

### Independent query leases and live watches — 26 September 2026

704e2b65 adds active BC3 encoding-callback cancellation tests. These establish
mid-encoding reservation debt cleanup and stale-output refusal. The race between
the final reservation check and Put publication is a separate OPEN boundary.
89db0dcc separates Fire-purpose exact query leases from Neighborhood demand and
watches live placements after leases end. Neither feature migrates generic
collision, AI, visibility or navigation callers; simulation residency stays OFF.

The retry supported build passed after four ambiguous unit Link assignments
were repaired; both failed and successful logs are retained. Fresh default OFF
passes 7430 assertions/163 cases (155 passed,8 explicit skips), broad preparation
368/13 (12 passed,1 skip), narrow 425/13 (11 passed,2 skips). Skip scopes are
enabled-only or mutually exclusive fixtures, not silently omitted failures.

Four installed 89db0dcc runs (16:25 stamp,actual binary hashes) report passed=true
and clean exit0. Root reviewed JSON and actor visual.png: both original white walls are present
with normal rendered geometry. This tiny artificial fixture is not retail or
Reforger parity.

- Default 162544 verifies Disabled with no admission.
- Actor 162621 verifies Fire request starts Pending despite Neighborhood Ready,
  then independently becomes Ready. An altered ray is Pending; invalid purpose
  is Invalid. Releasing Fire leaves Neighborhood leases intact. Moving the actor
  releases unmodified walls and returning reacquires them. Damage .25 survives
  as MustBeSaved; its watch makes coverage globally Unknown even at zero leases.
- Moved-watch 162744 moves wall 1002 from its authored bins to 100,100; neighborhood
  and a new query both become Unknown. A live-placement watch refuses stale
  origin-based readiness rather than inventing a new certificate.
- Capacity 162814 supplies 257 model groups; CapacityExceeded occurs at inventory
  rows 0/created 0/ownerPumps 0 before any partial readiness or wall admission.

Query Ready covers limited static terrain-object residency, not clear collision
or a hit-free ray. Legacy candidate enumeration still expands by a hardcoded 25m
object-origin radius and can omit large objects; broadphase/query consumers,
background AI publication and all-type eligibility remain OPEN. The current
approximately 6m wall fixture cannot close those gates. Headless readonly probes and automatic actor admission now pass below; generic
query/consumer completeness remains unverified. No new FPS, memory eviction,
multiplayer authority or whole-plan completion claim is made.

### Headless automatic actor admission — 26 September 2026

d24fe050 registers only readonly identity, simulation-status and fire probes for
the server; mutation and screenshot commands remain client-only. The supported
build passes, with 100 assertions/10 focused unit cases passing. Server runtime
uses the built dist PoseidonServer artifact (SHA256 recorded in provenance) with
installed-game files through -C. The unchanged installed graphical 89db0dcc stamp
is not the evidence for this server binary.

Headless OFF 163239 and ON 163331 both pass strict JSON checks and clean exit0.
OFF reports Disabled, ownerPumps 0, both walls absent, required 0 and renderer None.
ON reports automatic Ready with inventory 2/one prepared model/two regions/
leases 2, query 0/required 0 and renderer None. Both walls have authored Geom/Fire/View
geometry on CPU, visualResident=false, deferred=true, normalVB0; the fire probe hits
wall 1001. Admission follows actor position without ResolveObject, camera
residency or a manually queued query. Actual server/runner/mission provenance and
run logs/results/fixture hashes are recorded separately.

This is the original narrow static two-wall fixture. It does not prove
arbitrary modern model eligibility, multiplayer/full-save authority, full
collision enumeration or generic query consumer migration. Simulation residency
remains default OFF; large-offset query correctness and cancellation-token
publication visibility remain OPEN. The complete streaming plan stays OPEN.

### Transactional static Fire and cancellation visibility — 26 September 2026

524741ab adds a bounded owner Fire collision operation that checks the exact
query token before publishing contacts and supplements legacy candidates with
certified leased placements. It does not default-enable or migrate general
collision/AI/dynamic callers. Native DDS owner claims now reject cancellation
visible after final validation and before Put consumption. Physical canceled
cached bytes may remain until worker polling (32 visits) or Clear; there is no
owner store wait. Token metadata is capped at 65536 and inventory counters are
current logical accounting, not allocator RSS or every old reference.

The first Fire build passes, but its unit suite initially fails four fixture
type cases; the fixture is corrected to Primary only. Retry passes 1140 assertions/
24 cases. Publication guards build passes; fresh OFF 7628/176 cases (166 passed,
10 skips), broad 560/19 (18 passed,1 skip), narrow 617/19 (17 passed,2 skips).
Rounding final 78/7 and source-info/radius 205/20 targeted checks pass. Failed
unit output is retained separately rather than relabeled as a build failure.

Large-origin 170046 has no result: query Ready becomes collide Pending because
double .01 demand is not contained by the canonical smaller float .01 radius.
65d074e7 corrects mathematically nested centreline coverage and canonical
engine-float harness requests. Large-origin radius-fix 170935 then passes clean0:
wall origin z 1400/radius 206; legacy origin-grid query finds zero, the new Fire
operation returns 1001 exactly once, CPU-only/deferred/VB0 and required0. Ordinary
small-merge 171051 passes the combined Fire, actor cleanup/reacquire, mutation
retention and visual route. Root reviewed actual JSON; earlier white-box image
acceptance is tiny artificial content, not retail/Reforger quality. This closes
the demonstrated large-origin omission for the narrow operation; untouched
legacy generic consumers remain a separate OPEN gate.

### Bounded original DayZ source inventory — 26 September 2026

Offline terrain census reads the actual revision29 world: 2971218 placements and
2352 referenced models. The dense owner pose 5704.58,2523.37 within 250m has 3423
placements/408 distinct paths. At65d074e7 former1M/256 inventory caps were exceeded. The860fb1e2 checkpoint below replaces that inventory limitation; active work/class/watch gates still refuse production Ready.

Top nearby models include betulahumilis 260, picea3f 155,wall_feng_3=134,
wall_fen1_5=100 and misc_bench2=90. Two original private source members are
inspected: platform1_stairs_30_wallr (8 LODs) and wall_feng_3 (6 LODs), both ODOL54.
New --simulation-source-info reports parsed IR allowAnimation 0, finite positions,
authored Geometry/View/Fire meshes, no proxies/frames/weighted selections or
nonunit weights, and no parsed class/animated properties. Platform properties
include visual lodnoshadow=1 and geometry damage=no; feng geometry damage=no and
drawimportance=.1. Absent fields describe this parser representation, not source
completeness; no Shape convex adaptation, config constructor or world Ready is
certified. Original source hashes, tool versions/commands, private extraction
logs and parsed reports are recorded. Legacy terrain inspect fails revision29;
modern census/objects pass. No all-model parsing or retail content is committed.

The Normal DayZ route is completed and classified below; no performance acceptance is made. Production class/consumer coverage,
mutable state/MP authority, adaptive secondary views and comparable-quality
performance remain OPEN. Both prototypes and pressure/paging experiments stay OFF.

### Compact metadata and real DayZ convergence — 26 September 2026

860fb1e2 separates compact model metadata from bounded active work. Preflight is
4M authored placements,8192 referenced models,4MiB fixed model records. Existing
landscape placement/cell storage, allocator overhead, sparse map nodes, watchers,
Shape/source payload are excluded. Active models remain capped256; sparse lease
debt erases zero-count rows. Initial build fails fixture syntax/getter errors;
repaired retry and focused1218 assertions/30 cases pass. Simulation stays OFF.

Metadata-capacity173048 passes clean0:8193 models refuse before partial inventory
(rows/prepared/created0). Active-capacity173134 passes clean0: complete257 rows/
models/32896 bytes accepted, then active work refuses before preparation/creation.
Actor-lifecycle173733 passes clean0: two rows/one model/128 bytes, sparse leases2,
independent Fire1001 before/away, wrong-ray Pending, cleanup/reacquire, .25
MustBeSaved/globalUnknown at lease0, one final logical-only identity. Tiny static
fixture only, not arbitrary production classes, multiplayer or generic consumers.

DayZ compact-metadata173918 passes the metadata diagnostic/clean0:2971218
placements/2352 models fully inventoried, metadataComplete true,301056 fixed
record bytes. Overall simulation CapacityExceeded, activeModelRefusals1,
active/prepared/leased/created0,unknownModels5,watches16384/live3481.
Complete metadata is not world Ready; eligibility, active scheduling and complete
mutable-instance observation remain OPEN.

Correct causal classification of default-publication-correctness171220:
Normal400m camera traversal/return, budget20000/resident19923, not stationary
20k-to4k budget shrink/restore. Geometry380668076→727994288; textures2467033284/
2236→2817465716/2596. Compact Normal route similarly grows geometry380668076→
781468516 and textures2504779716/2253→2904934452/2654. Initial cold warming/new
coverage and normal LOD variation are plausible; neither leak nor performance/
no-regression acceptance is established.

Resource-convergence174103 exits clean0 but is NOT PASS: passedfalse/
PendingWarmup, no ten-sample quiet baseline within30s including three returns.
Return2→3 geometry+8903008 bytes and textures+35474656 bytes/42 entries exceed
declared gates; capacity stable/retired0. FixedLOD1 narrows demand variation but
does not establish retained-memory convergence. Private/RSS samples are separate
from GPU accounting. Original launched provenance is authoritative; subsequent
runner edits do not describe this run. Leak/performance attribution remains OPEN.

At the860 checkpoint StageF bounded allocation-only report was unbuilt; the995/abfd validation below supersedes that source-only status.
Deduplicated per-LOD referenced allocations must remain distinct from global
pool live/capacity/retired bytes; no selected-LOD usage or default paging claim.
Whole plan, production consumers/classes, state/MP, secondary/adaptive visuals,
and comparable-quality performance remain OPEN. Reforger is workload evidence,
not compatibility ground truth.

### Bounded allocation attribution and predicate correction — 26 September 2026

9950974c adds explicit-request allocation attribution; ordinary frames do not
scan the model registry. Request payload is an optional sidecar, not extra
diagnostic vectors/counters in every resource operation. Owner-drained snapshot
and nonblocking polling preserve producer/renderer ownership. Rust tests3 cover
sharing/deduplication, missing/invalid inputs and truncation; FFI ABI1 passes.
Supported/stamped builds pass.

DayZ175633 exits clean0 and passes narrow Converged/persistent quiet: HAL aggregate
remains in raw samples and return-growth comparison, but is excluded from quiet
because it includes transient staging/in-flight resources. Return2→3 geometry
+4316028, textures+21844000/+10 pass unchanged declared growth thresholds. This
is diagnostic convergence at one fixed-LOD route, not FPS/process-memory/all-world
acceptance. The old174103 PendingWarmup label used a different quiet predicate:
all29 initial quiet samples have constant live/capacity geometry and object
texture bytes/count; only HAL varies4429338992–4430388736. Its actual return-growth
gate failures remain; PendingWarmup alone did not prove objects kept loading.

Original995 report is Ready/valid/truncated, requested256/visited179,639 LODs/
unique meshes,1119 sections and96360444 unique vertex/index bytes versus global
pool733798952. The registeredModels750/missingModels77 labels conflate cached
ineligible shapes with retained GPU models. Source unconditionally caches
WGR_INVALID_MODEL for rejected shapes; this strongly explains missing77, but
row-only output cannot independently count those exact77. No retired asset-loss
claim follows.

abfd4131 separates registry entries/visits/skips and passes only valid producer
IDs. Corrected181509 exits clean0 AllocationDiagnosticOnly, convergenceMeasured
false; no repeated traversal/performance pass. Registry683/visited256/skipped68,
models requested188/visited188,missing0,673 LODs/unique meshes,1231 sections,
missing meshes/invalid ranges/unresolved names0. Union81965296 vertex+13184856
index=95150152 bytes; global pool live380859292/cap637534208/retired0.
Truncation is explicit: bounded registry subset, not top-ranked assets or full
scene. Per-LOD rows can overlap; union bytes are retained allocations, not selected
draw usage or reclaimable memory. All experimental defaults remain OFF.

abfd also adds pure bounded complete-compiled-ODOL static-source envelope evidence.
Supported/stamped game/test/server build and475 assertions/28 cases pass, including
seven new cases with actual CPUOnly adaptation. This consults no bank, config,
proxy resolver or Object; it is neither final constructor evidence nor world
Ready and has no runtime pipeline integration. PlainRouteProbe remains unbuilt/
unused pending. Production eligibility/query consumers, MP/state authority,
broader comparable-quality performance and conditional paging remain OPEN.

[Tidewater research](TIDEWATER-STREAMING-REVIEW-20260926.md) is retained as deferred
architectural research, not implementation/runtime evidence. Waves remain parked.
Reforger stress content remains workload evidence, not compatibility ground truth.

### Audited raw source prerequisites and opt-in preparation — 26 September 2026

20ab9ead carries producer/version/revision, declared/decoded LOD coverage and
observed-bit raw directory/skeleton/keyframe/bone facts. Previously skipped motion
payload and omitted directory facts cannot be inferred absent from adapted IR.
Old parsed-IR absence reports above remain qualified. Optional tagged60-byte
ModelBlob footer retains base1/cache key; exact old base blobs load with source
audit Unknown. Unknown/missing/motion facts fail closed for static consumers.

Combined game/tests/server/tools build passes. Initial prerequisite suite fails
one positive quoted-numeric empty-class fixture:763/764 assertions,46/47 cases;
that failed log is retained. Corrected integration1011/61, AST004 oldbase cache
compatibility3551/11 and pathguard1023/63 pass. Stamped build/deploy pass;
installed20ab9ead19:32 provenance matches compiled source.

ODOL numeric-envelope sidecar is opt-in/default0, computed only for exclusive
parse before Ready. External aliases and late Queued/Parsing/Ready requests are
refused. Live plain-route class probe is one current owner operation, not a
config-epoch certificate/cache or durable world Ready;127/128 path boundary
checks prevent truncated class lookup. Raw source audit is decoded-source
evidence, not actual class/constructor/final object proof.

Tiny fixture lifecycle193346 passes clean0: coldReady2 rows/one model, two
leases, independent Fire, cleanup/reacquire, .25 MustBeSaved mutation retained/
globalUnknown at lease0. This does not extend eligibility to arbitrary retail
classes or establish multiplayer/generic consumer integration.

DayZ193954 passes clean0 AllocationDiagnosticOnly, convergenceMeasuredfalse.
All27 samples report sourceEnvelopeBytes/Scans/Published/Ms0: default-OFF sidecar
does no measured work. Allocation report is valid/truncated: registry683/256
visited/70 skipped, valid requested186/visited186/missing0; union98160624 versus
pool380859292/cap637534208. Root viewed normal street screenshot, not visual
compatibility ground truth. No repeated traversal or performance claim follows.

Offline source-audit eligibility inspects the two original ODOL54 walls:
producer1/revision54/geometryCoverage1/observations15, declared=decoded8/6 LODs.
Observed directory animation/skeleton/keyframe/bone counts are0; zeros certify
absence only with corresponding observed bits. Actual config/class/constructor/
world readiness remains separate. Tool/provenance/source reports are hashed.

[Geometry streaming research](GEOMETRY-STREAMING-RESEARCH-20260926.md) records
the staged Nanite-like investigation, not implemented pages or accepted gains.
[Tidewater research](TIDEWATER-STREAMING-REVIEW-20260926.md) remains deferred;
waves stay parked. Next compact source-summary foundation is in progress without
validated ledger claim. Full plan OPEN, experimental defaults OFF, Reforger
stress evidence not ground truth, production/MP/performance gates unresolved.

### Compact source summaries and bounded selected diagnostics — 26 September 2026

8a87b268 adds a private fixed source-summary value <=288 bytes with generation,
source path and config basename only. No radius, IR/config pointer, disk freshness,
actual constructor, final bounds or Ready is certified. Summary and numeric
envelope are published together from the same exclusively owned original parse;
a live class observation is repeated at the owner operation rather than cached
as a durable config verdict.

Selected diagnostics own eight fixed records with30-second TTL independently
of simulation caps. They retain no IR/geometry/Take result, required pin or
simulation lease. All three DropStale/reset paths and the joined World owner
guard preserve only explicit unexpired pending demand while simulation is OFF.
Expiry removes demand; ordinary camera DropStale retires IR. Lazy metadata
vector capacity remains charged until Reset, not automatically freed at TTL.

First integration build fails two API errors (missing Has method/wrong response
name); repaired integration/boundedness/request-token/terminalguard/scan-baseline/
stamped builds pass. Summary isolated38/4 passes. Diagnostic isolated fails
36/37 assertions1/2 cases, composite1161/1162 assertions68/69 cases: an ordinary
request could borrow an old diagnostic pending record. Fixed monotonic explicit
parse token in lazy sidecar (8bytes/slot), inventory generation/identity/token
checks prevent this. Normal fresh requests/DropStale clear0; Reset changes
generation but never resets token sequence, UINT64_MAX refuses, and the ordinary
default path never advances it. Final1182 assertions/69 cases pass.

First runtime actual203600 has no result: runner assigns an object to its typed
PowerShell Capacity switch. Engine cap refusal was correct; finally stops owned
game. Failed runner log/provenance are retained. Corrected retry203722 on unchanged
installed8a87b26820:35 passes clean0; launched corrected script hash in provenance
is authoritative even though correction follows the source commit.

Original nine-model ODOL7 fixture produces eight LatestSnapshot records with
sourceEvidence/plainSourceFacts true; actual immediate class observation is
EmptyClassNow for all eight. Ninth request Capacity, late request TooLate, TTL
Expired/diagnosticActivefalse. Simulation stays Disabled/ownerPumps0, resident/
required0. After expiry ordinary camera recenter yields readyPayload/parseReserved0,
while sourceEnvelopeBytes2880 remains as charged metadata capacity (eight scans/
publications, .0486ms measured fixture scan time only). No eligibility Ready,
visual/performance or actual DayZ production claim follows.

Offline original-format Tool acceptance is separately hashed, not runtime
quality evidence. Next production eligibility measurement remains pending.
Whole plan OPEN, experimental defaults OFF; research and parked waves unchanged.

### Original DayZ offline source summaries — 26 September 2026

5dc55f92 extends only Tools diagnostic/header tests; normal engine remains
unchanged and installed8a87b268. Offline11-node build and224 assertions/15 cases
pass. Tool was built at9226b63a plus two changed files before source commit;
actual tool SHA4e412c954068b92cce2f16862898848c825f52a53f34f35945b2cbfde32bbd35
is recorded, not a stamped5dc game/tool provenance claim.

Two unchanged original ODOL54 DayZ walls are copied to short extraction identities
preserving original basenames and bytes (original/copy SHA equality in provenance).
With synthetic generation1, platform1_stairs_30_wallr produces SourceEvidence,
payload97176/sourceRadius4.76501369 and SourceFacts/config basename matching;
wall_feng_3 produces77596/1.89691389 and SourceFacts. These are decoded-source
envelopes and compact summaries, not actual live CfgModels/class/constructor,
placed world final bounds, Ready or performance evidence. Extraction identities
are not production inventory identities and generation1 is not content freshness.
Scope is only these two models, not the full DayZ corpus.

Positive actor warming is a new implementation in progress/unverified. It is not
promoted from offline facts. Whole plan OPEN, experimental defaults remain OFF.

### Positive leases for already-loaded actor geometry — 26 September 2026

9b13b51c retains already-loaded actor-neighbourhood objects through bounded
positive leases even when full source/model inventory cannot establish Ready.
Independent live entry witnesses establish only those positives; unknown groups,
global watch overflow or active-model cap cannot become partial query Ready.
It reuses sparse lease cleanup and does not cold-create/load models or claim
collision clearance. Review corrected full-group hot scanning and pending-prefix
starvation before final build. Initial193-node build/480 assertions35cases and
final21-node stampedbuild/1450 assertions75cases pass; installed9b13 at21:05.

First runtime210610 has no result: valid descriptor enqueue returns Pending,
runner incorrectly expects Capacity rather than reading separate query status.
1b8a03a0 corrects runner/pushed safety; installed engine remains9b13.
Retry210728 passes clean0:257-model fixture retains two positive leases despite
CapacityExceeded, ReadyRegions0/FireCapacity, prepared/created/source-envelope0,
required0; actor departure releases them. This does not close cold integration.

Actual DayZ210842 passes positive-resident diagnostic/clean0 at normal budget20000.
All2971218 placements/2352 references are metadata-complete; loaded resident19923
has303 positive leases. Camera9704 departure loads19983 elsewhere and preserves
303 logical-only identities; actor12000,12000 releases303 to0. Global queries stay
CapacityExceeded with noReadyRegions. Watch cap16384 is reached, but independent
bounded witnesses preserve only observed loaded positives; they do not certify
global completeness. Cold source/model preparation/creation/envelope work and
required pins remain0. Last pump samples .0583/.0819/.0042ms are point diagnostic
observations, not FPS/no-regression proof. Root viewed normal street screenshot;
stress-content appearance is not compatibility ground truth.

Full lifecycle211114 passes clean0: existing cold actor fixture admission,
visual promotion/demotion, Fire, .25 mutation and saved-state release retention.
Default-OFF211128 passes clean0 Disabled/ownerPumps0/coldwallsabsent.
These preserve narrow prior behavior, not arbitrary retail classes/MP or generic
consumers. Cold integration remains pending; whole plan OPEN/defaultsOFF,
Nanite/Tidewater research deferred and waves parked.

### Opt-in cold positive actor admission — 26 September 2026

d3f5fe8e adds selected plain original-ODOL cold actor-channel0 binding through
async conversion. Separate WGR_SIMULATION_POSITIVE_COLD=1 also requires simulation
enabled; both remain OFF by default. Old request/charge transfer is preserved;
ShapeBank exception ownership uses RAII, tested with actual throw/cache weak-null/
retry. Paired worker SourceEvidence avoids repeating source vertex scans on the
owner, retaining the stricter32-LOD adapter cap. All-target integration202 nodes,
reviewed17 and stamped18 builds pass; combined1805 assertions/93 cases pass.
Installedd3f5fe8e21:46 matches source provenance.

Cold214638 passes clean0 on original authored ODOL7 content:257 referenced models,
only two eligible rows near actor1190, camera100 never visiting that area.
Requested/Prepared/Created2,Refused/Interrupted0,leases2; identities1001/1002 have
visualfalse/normalVB0 and Geometry/Fire/View roles true. GlobalCapacity persists,
not fullReady/collision clearance. Owner bank-tail last.0143/max.0265ms measures
that tail only, not FPS or a hard complete owner-frame bound. Actor departure
removes both objects and all jobs/facts/payload/shape charges exactly to0.
Artificial original content is not retail source/class/general eligibility proof.

DefaultOFF214804 and prior full-lifecycle214821 pass clean0: Disabled/pumps0/
coldwallsabsent; existing actor/query/camera/movingactor/Fire/mutation.25 saved
retention unchanged. No default enablement or comparable-quality performance
acceptance follows.

Actual DayZ inventory83912800 chain is running without classified result at this
checkpoint; production source/path/liveclass/constructor/cold placement proof
remains OPEN. Next gates: real production refusal/success evidence, matched
normal traversal performance, full consumer Pending/Unknown fallback, headless
cold ownership and wider save/MP state. Hierarchical geometry paging stays
conditional on measured benefit; Nanite/Tidewater research deferred, waves parked.
Whole plan OPEN and experimental defaults OFF.

### Actual DayZ source-bound cold actor chain — 26 September 2026

First actual directory215052 has no result: WGR_OBJECT_STREAM_PBO=0 leaves
archive sources NotLoose, with105 requested/refused and0 prepared/created/
source-envelope scans. This is incorrect source transport configuration, not
live class refusal. Failed samples/provenance/logs are retained.

Retry215427 passes clean0 on unchanged installedd3f5fe8e. Existing archive source
support is opt-in WGR_OBJECT_STREAM_PBO=1; explicit POSEIDON_MODEL_DDC=0 ensures
original decoded source audit for this proof. Production defaults are unchanged.
Camera/player stays10000, budget20000/residentdesired19985; target83912800 starts
absent, then actor remote5711 demand admits the original inventory path
dz\structures\residential\platforms\platform1_stairs_30_wallr.p3d. No ResolveObject,
manual source diagnostic/model whitelist or camera visit establishes residency.

Actual target is plain source-bound CPU-only: radius4.731777191, visualfalse/
normalVB0 with Geometry/Fire/View true. Real stream_fire_probe hits83912800 (and
nearby83912843). GlobalCapacity/ReadyRegions0 and transactional FireCapacity
remain unchanged: no fabricated contact list or partial world Ready is published.
Final105 requests/18 prepared/81 created/72 refused,105 scans/31 publications
are bounded observations, not whole-corpus eligibility acceptance. Maximum owner
bank tail1.4754ms is not FPS or a hard complete owner-frame bound.

Actor departure retires target identity and all cold work/facts/payload/shape
charges to0;422 previously loaded positive leases near player10000 remain.
This is cold-debt cleanup, not global lease0 or freeing metadata capacity.
Real inventory/source/current class/actual CPU constructor/contact/retirement
chain now has one partial production proof. Promotion/mutation/save/MP, wider
classes, fullquery completeness and matched-performance gates remain OPEN.
Root-owned runner is uncommitted at this checkpoint; exact launched scriptSHA
and world/binary provenance are authoritative. Whole plan/default gates unchanged.

### Production cold promotion and mutation retention — 26 September 2026

First lifecycle215804 has no result because runner demands modelVB0 after camera
demotion. Engine correctly sets VisualResidentfalse. LandSave instance eviction
calls SceneObjectRemoved and SetVisualResident(false); shared Shape/bank model
buffers have no inverse deferred-visual teardown contract. Fix changes runner
expectation only, not engine code. Failed log/samples are retained.

Corrected220125 passes clean0 on unchangedd3f5fe8e: actual83912800 starts CPU-only
normalVB0, dense camera promotes visualtrue/normalVB4 with physical roles/contact
preserved, then camera departure demotes visualfalse while shared modelVB4 remains.
Existing-object damage mutation.25 sets MustBeSavedtrue. Actor departure preserves
same target .25/visualfalse/saved, with cold work/facts/payload/shape charges0 and
required pins0. This is actual production in-memory state retention, not disk
save/reload, multiplayer, fullquery Ready or performance acceptance.

Model-buffer cache retention is explicit: retiring an instance or cold job does
not prove GPU geometry reclamation. Per-asset geometry demand/cache policy stays
OPEN pending measured retained-LOD usage and safe shared ownership. Headlesscold
run remains unclassified at this checkpoint. Wider consumers/classes, diskstate/
MP and comparable-quality performance remain OPEN/defaultsOFF.

### Headless cold ownership — 26 September 2026

Headless220322 passes on freshly built d3f5fe8e PoseidonServer.exe, actual server
artifact hash and mission/runner provenance recorded. Renderer is None; original
ODOL7/257-model fixture requests/prepares/creates exactly two remote CPU-only
identities with Geometry/Fire/View, normalVB0 and real fire1001. GlobalCapacity
remains, with no fullReady/query clearance or required pins.

Actor700,900 removes both identities and leases/coldwork/facts/payload/shape
charges to0. Server exits clean0 under the owned game lock. This verifies one
narrow server cold lifecycle independently of graphical residency, not multiplayer
authority, disk-save restoration, arbitrary retail types or performance.

Next owner-bank timing/alpha attribution is planned instrumentation only, not
implemented/accepted evidence here. Geometry-cache demand, broad consumers/state,
MP and comparable-quality performance remain OPEN; experimental defaults OFF.

### Latency attribution and normal traversal screening — 26 September 2026

ddf890a1 adds attribution only, no behavior/source format/class policy change.
Clock/counter work is profile-active only; cold-bank counters require opt-in.
Initial build fails ambiguous NowUs declaration in wrong namespace; repaired
14-node and stamped18-node builds pass,7041 assertions/169 cases pass.
Installedddf22:15 matches source/binary provenance.

Cold-bank221705 passes class/admission/retirement diagnostic clean0. Final26
bank calls/maxBank2.1048ms/maxTail2.2402ms exceed a hard2ms promise: existing
budget is soft, not guaranteed owner-frame latency. Bank scope includes relabel,
table allocation/build/exception ownership; source audits, conversion and final
tail are separately attributed. Actual t_fraxinusexcelsior_2w alpha49.3/total50.1ms
has sourceRead11.895/decode3.904/hist11.033/shape21.210ms, blockpath1.
SourceRead includes decompression and remaining time is unassigned. This is a
measured cause candidate, not optimized/accepted performance.

Normal-route Disabled221845 and Cold222141 pass runner/clean0 using same installed
binary and PBO1/DDC0. Actual route frames:
Disabled651 median26.132/mean56.6506/p95199.244/p99633.202/max1423.016ms;
Cold727 median25.380/mean49.7615/p95166.571/p99496.330/max1020.984ms.
Both traverse with severe hitches, dominated by drw:init/terrain/swap. Ordered
single pair/cache/order/phase variation does not support improvement or regression
acceptance, no vanilla parity. Both coldCreated/sourceScans0: Cold means feature
ON with positive-loaded retention on a camera-covered route, not cold-job timing.
Raw frame samples, phase results/provenance and acceptedfalse comparison are hashed.

Save-roundtrip run remains unclassified/pending here. Shape-loop optimization
source is being prepared but unbuilt; no implementation/gain claim follows.
Hard owner-bound, shared geometry-cache demand, source worker coverage and broad
comparable-quality performance remain OPEN. Defaults OFF/whole plan OPEN.

### Actual cold target same-world disk save — 26 September 2026

DayZ222342 passes clean0 on installedddf890a1. Actual initially cold source-bound
83912800 has required0, then promotion/demotion and existing-object damage.25
MustBeSaved retention after actor departure with cold work/facts/payload/shape
charges0. triSaveGame writes a private6968-byte FPS file, SHA256
76e0edea5d00140440c6cc6484725d3e2425116dbd19bcbee3fbfad0f208fc73
as verified against the actual saved file.
Existing-object .75 mutation provides control; triLoadGame in same world/private
profile restores target ID/model/frame/.25/MustBeSaved and actual fire contact.

Loader required count3 is recorded, not silently claimed0 after restoration.
Shared modelVB4 retention continues; restored disk state is not geometry cache
reclamation. This closes one actual production same-world SaveBin/LoadBin
roundtrip, not fresh-process/multiplayer/performance or general state coverage.
Only runner save mode changed; source/binary remainsddf. Shape-loop optimization
build remains ongoing/unclassified. Whole plan/performance/MP/wider gates OPEN.

### Alpha shape helper measurements — initial pre-integration screening

Isolated benchmark at f0d0a0c5 compares an independent pre-optimization reference
with the candidate in separate translation units, O2/no LTO,2048² synthetic data,
nine alternating-order batches of three calls. Allocations are excluded. Exact
measured/meanClearNeighbours/clusteredClearFrac/partialBandWidth equality is checked.
Extended suite is eight patterns with stride1 compact alpha and stride4 RGBA.
CSV p95 column is largest of nine batch values, not a full-game frame percentile.

Initial compact partial pattern regresses33–39% and opaque29–50%; candidate is
rejected. Repair1 improves some patterns but sparse .1% clear regresses91–126%;
that candidate is also rejected. Repair2 adds opaque fast exit and partial
branch loop, then extended+confirmation results show no reproducible material
median slowdown in this limited screening. Sparse compact deltas +.029/- .064ms;
RGBA -.068/+.034ms change sign. Other tested eligible synthetic medians generally
reduce about10–60%, not a fixed no-regression or whole-texture/FPS guarantee.

All benchmark sources, executables, compile logs, result CSVs and initial
provenance hashes are retained. Initial provenance describes its capture phase;
later repair artifacts have their own hashes, not a stamped source-build claim.
At this initial screening checkpoint parity build and runtime/actual-asset results were still pending. The following c04ae3f6 section records subsequent passing validation; performanceAccepted/defaultReady remain false, full plan OPEN/defaultsOFF.

### Alpha repair2 and production fresh-process restore — 26 September 2026

c04ae3f6 repair2/freshsave22-node build, stamped18-node build and61695 assertions/
175 cases pass. Actual installed stamp is c04ae3f6+dirty22:46 (docs dirty at build);
exact executable/renderer hashes in both provenance records identify this binary.
Later histogram candidate source edits are unbuilt and NOT included.

DayZ224706 passes both processes clean0. Source-bound target83912800 initially
cold/unpinned is promoted/demoted, damaged.25 and saved; .75 mutation controls
restoration. First process exits, new PID starts with same private profile/world,
target absent before LoadGame/required0, then actual disk load restores ID/model/
all12 frame coefficients within1e-6 (actual delta0), radius4.731777191, damage.25/
MustBeSaved, CPU Geometry/Fire/View, deferred visual/normalVB0 and real firehit.
Loader pin count3 is retained honestly. Actual6968-byte save/provenance hashes
are recorded. This closes one real production fresh-process save gate, not
multiplayer, broad state, migrated gameplay consumers or fullworld Ready.

Same actual tree log records shape21.210→10.458ms versus prior ddf, sourceRead
11.895→11.135/decode3.904→3.181/hist11.033→9.092ms, alpha49.3→34.9/model50.1→35.9ms.
That is one ordered local-asset observation with OS/cache/other-part confounds,
not FPS improvement or comparable-quality no-regression acceptance. Synthetic
parity and accepted local functionality do not enable experimental defaults.
Later histogram candidate remains pending/unbuilt; full plan/performance/MP/
general-state/consumer/defaultReady gates OPEN.

### Histogram candidate screening — initial pre-runtime checkpoint

f5e51c5e final histogram helper/test source builds20 nodes and passes111276
assertions/180 cases. At this initial checkpoint stamped18-node build/runtime were pending; the following installed proof supersedes that pending state. Service agent design remains separate/not included.

Candidate0 regresses many patterns35–84%, rejected. Repair1 bounded chunks improve
production stride1/4 but generic stride3 sparse-blend regresses18–20% in two runs,
also rejected. Final repair outlines the original generic branch, shares policy
and uses65536-sample32-bit chunks (maximum sum16711680, checked safe). No allocation,
classification policy or default change follows. /O2 makes reductions vectorizable;
no disassembly evidence establishes specific SIMD instructions or speed guarantee.

Independent baseline/candidate separate translation units, clang-cl22.1/O2/noLTO,
2048² pixels, nine patterns×strides1/4/3, nine alternating batches of three calls
check every AlphaStats metric exactly. Max-batch column is largest nine batch means,
not a game-frame tail distribution. Final generic sparse-blend ratio.995 reverses
prior1.19; generic medians fluctuate within about1.6% except3.3% speckles reduction.
Production medians mostly improve; opaqueRGBA+.054ms/1.18%, sparseRGBA+.008ms/.16%
are small observed costs, not a fixed no-regression pass. Actual source/header/
executables/results and phase-specific provenance hashes are retained.

performanceAccepted/defaultReady remain false; actual-asset/runtime gates pending.
No gameplay consumer migration, no service-model-cap or fullclearance proof.
Whole plan OPEN/defaultsOFF.

### Histogram actual-asset checkpoint — 26 September 2026

Final stamped18-node build passes. Actual f5e51c5e+dirty23:04 installed stamp
reflects docs dirty at build; exact binary/provenance hashes identify the tested
source. Later query-model-debt and worker-alpha edits are unbuilt/unverified and
NOT included in this binary. Final repair2 benchmark provenance is separately
hashed, including phase-specific sources and executable.

DayZ230454 passes clean0: actual83912800 cold admission has CPU Geometry/Fire/View,
normalVB0 and real fire contact; globalCapacity remains with no fabricated Ready
or required pin. Actor departure removes target and cold work/facts/payload/shape
charges to0. This accepts narrow correctness/integration, not general gameplay/
world coverage or performance.

Actual fraxinus row read11.656/decode3.477/hist3.221/shape11.004ms, alpha30.8/
model31.7ms; earlier c04 hist9.092/shape10.458/alpha34.9/model35.9 and ddf
hist11.033/shape21.210/alpha49.3/model50.1. Histogram component is lower in these
ordered local observations, while other parts change. OS/cache/order and limited
asset selection prevent FPS or no-regression acceptance. Exact classification
111276 assertions/180 cases remains verified, defaults unchanged.
performanceAccepted/defaultReady false, full plan/production consumer/MP/broad
state/performance gates OPEN. Future source work has no accepted evidence here.

### Entry-owned query model debt — 26 September 2026

3db4159b193-node/stamped18-node builds and111374 assertions/181 cases pass.
Actual installed3db+dirty23:20 binary is identified by exact provenance hashes;
IO archive-lease edits and weak metadata work occur later and are NOT included.

Runtime232003 passes clean0 on original CPU-wall fixture. Actor+Fire interests
share one charged model and two deduplicated Entry debts; independent query
release preserves actor1/2. Actor departure releases borrow0/0 and both IDs.
Reacquire then .25 mutation preserves one MustBeSaved survivor, but joined
cleanup releases model debt0/0, globalUnknown/liveWatcher1/required0. Existing
query/fire/camera promotion/demotion checks pass. Ready checks actual model borrow;
positive observation cannot erase debt. This remains the <=256 full producer/
same Shape accounting, not weak metadata, cap relaxation,257Ready, full DayZ
coverage, multiplayer or performance acceptance.

Source freshness prerequisite remains explicit. BankReadRequest path/offset/
length snapshot opens a new stream and does not retain bank access lifetime/file
identity; bank unload or pathname replacement can outlive current world/store
generation. Existing tests prove captured decoded facts/narrow functionality,
not an immutable/fresh-source theorem. At this3db checkpoint Windows archive-read lease work was pending;
defaults PBOOFF unchanged, Linux immutable behavior unproven fallback. Pre-upload
alpha-facts experiment remains frozen with no implementation.

Weak registered metadata is later work under development/unverified, not this
binary. Whole plan/defaultReady/performance/MP/general consumers remain OPEN.

### Optional physical archive lease prerequisite — 26 September 2026

1e0b7611 full582-node and stamped18-node all-target builds pass, clean installed
stamp23:37 identified by provenance binary hashes. Required Windows capture can
retain an independent read-only archive handle, compare mounted physical file
identity and bounds, and validate the worker reader against that identity.
FILE_SHARE_READ denies writes/deletion for the lease lifetime. This is optional:
existing calls keep the default unleased capture; POSIX required capture refuses.
Source freshness at all production call sites remains OPEN, and prepared-alpha
facts have no implementation in this binary. Later weak-metadata/alpha work is
not included.

Core exact archive-lease tests pass71 assertions/3 cases using actual Windows
OS operations. Core union194 assertions/24 cases has20 passed/4 external-data
skips. PoseidonTests IO union144 assertions/22 cases has19 passed/3 skips and
contains no archive-lease tests; it is not OS-lease proof. Streaming/alpha/source
borrow111374 assertions/181 cases passes. Default-disabled233709 runtime passes
clean0 with ownerPumps0/required0/queryborrow0/0 and both remote walls absent.
This checks normal unleased compatibility, not required-lease game activation.

### Existing PBO texture baseline before prepared-alpha changes — 26 September 2026

Same installed1e0 ordered ON234444/OFF234559 runs pass clean0, view1280x720/vd2000,
with19985 camera residents before/final. Original per-run script hashes preserve
the later PBOTextures runner option. No capture.json exists; no frame percentiles,
FPS or no-regression acceptance is available.

Fraxinus ON read10.156/decode6.382/hist10.291/shape10.577ms, alpha37.7/model38.6;
OFF7.069/4.039/2.898/10.158ms, alpha24.4/model25.0. ON retains320 prepared-texture
entries/536868112 capacity bytes and peakconversion467008340; OFF0/0 and171011444.
These are existing-pipeline measurements, not new prepared-alpha evidence or
causal timing attribution: order/cache/other phases confound the single pair.

Both targets retire with required0 and cold work/facts/payload/shape charges0;
historical created/released are81/81 ON and77/77 OFF. Both retain422 positive
leases for the normal actor already at the starting location, not globalzero.
DayZ2971218 placements/2352models, watch16384 and globalCapacity remain; no full
world Ready. Default PBOtextures0 remains, performanceAccepted/defaultReady false.
Whole plan OPEN; geometry cache, generic consumers, source freshness, broad
state/multiplayer and performance gates remain.

### Source-bound prepared alpha and weak registered census — 27 September 2026

b57a0b90 adds optional worker alpha histogram/shape facts paired with the original
packed top mip and Windows physical archive lease. Consumer checks current
archive/member, format, dimensions, top bytes and exact original header offset;
source-bound counters are independent of GPU/CPU upload path. Unsupported identity,
format/layout, cancellation or analysis failure retains ordinary classification.
Known C++ capacities plus a64-byte control-block guard are charged, shared leases
deliberately per entry: conservative retained-capacity accounting, not allocator
RSS or universal source immutability. Model source capture remains unleased.
PBOtexture preparation stays OFF by default.

1afb85d8 separates weak registered model census from selected strong borrowed
residency, keeping selected work/charges bounded. ParentR finite and parent
QueryPolicyRevision alone do not certify actual CPU points or authored per-LOD
bounds: adapter copies ODOL min/max/centers/radii, and mutable vertex/LOD accessors
can bypass parent revision. Registered metadata is not physical clearance.
Bounds/mutation/source-freshness and broader consumer gates remain explicit.

Full643-node/stamped18 builds pass, clean installed1afb00:17 verified by actual
provenance hashes. Targeted26619 assertions/25 cases24pass1TTLskip; fresh facts
budget/TTL283 assertions/2 cases passes; combined137683/206 has204pass2skips;
CoreIO194/24 has20pass4 external-data skips. Exact prepared-facts parity/budget/
TTL correctness does not establish whole-game performance.

Actual DayZ002219 passes clean0: cold facts puts283/hits62/miss0/cancel0,
requiredPreparedAlphaFactstrue,19985 before/final residents. Prepared texture
entries303/capacity536787952, peakconversion471335268. Target CPU Geometry/Fire/
View/VB0 and real contact pass, then actor-away target absent/required0/cold work/
facts/payload/shape charges0. Historicalcreated/released81/81 but422 normal-actor
positive leases remain. DayZ2971218 placements/2352models/watch16384/globalCapacity
still refuse full Ready. This is actual facts use/correctness, no FPS or
no-regression acceptance.

Registered first001734 runtime has no result.json: exact query and collision
returned Pending, while the runner prematurely expected Ready. Do not classify
as missing geometry or accept registered runtime. Root owns the repaired retry;
matched prepared-alpha OFF arm remains pending at this checkpoint.
Whole plan OPEN/defaultReady/performanceAccepted false; shared model buffers,
geometry release, broad state/MP and production consumers remain open.

Corrected registered002518 runtime now passes clean0 on unchanged1afb. Waiting
for the exact Fire channel gives Ready/fire1001, one registered model/two Entry
borrows/1072864 scheduling charge. Camera departure causes weak expiry Unknown;
actual .25 Saved survivor remains while final query/registered borrow and charge
return0, required0. Final validation budget refusals103 are reported. This closes
the narrow257-model original fixture warm query/debt lifecycle, not arbitrary
geometry-bound validity, full retail coverage, collision clearance or performance.

Matched new OFF002410 also passes clean0 on the same binary, equal cold resident
19985/livegeometry230706772/cap318767104/textures1262855596/count1573/backend2744302016.
OFF peakconversion171011444 versus ON471335268; prepared textures/facts0 OFF.
Fraxinus ON alpha23.6/model24.3 versus OFF24.7/model25.6ms is a single ordered,
cache-confounded component observation, no FPS/no-regression acceptance. Actual
ON303 prepared entries/536787952 capacity and cold283puts62hits remain scoped.
Future advisory-capacity source is unbuilt and not in this tested binary.
Defaults OFF/wholeplan OPEN/source-bound and shared-buffer limits remain.

### Capacity screening and inspected geometry overlap — 27 September 2026

0f0b2559 builds259 nodes/stamped18 pass. Rust overlap6 and ABI8 tests pass,
including rejection of oldABI12; C++86559 assertions/95 cases has93pass2skips;
fresh1MiB/TTL.01 alpha316 assertions/2 cases passes. ABI13 adds104-byte matched
summary layout. Overlap counts unique live meshes only when distinct inspected
(model,LOD) pairs reference them, never duplicate sections or repeated same-model
requests. Truncated subsets stay partial and ownershipCoverageCompletefalse;
no global exclusive/reclaimable bytes or selected-use interpretation.

Capacity screening checks conservative chain+facts charges before expensive
optional analysis, grants no reservation, and final Put recomputes actual charge.
TTL not yet swept may conservatively skip facts; ordinary chain/classification
fallback remains. This is an opportunity, not unconditional loading/FPS benefit.

Actual003805 passes clean0 with671 capacityskips/284 factsputs/62 hits/0 miss/
cancel. Resident19985/livegeometry230706772/cap318767104/objecttexture1262855596/
1573/backend2744302016 matches predecessor. Peakconversion428760152,319 prepared
entries/536869024 capacity bytes; fraxinusalpha26.1/model27.1ms does not demonstrate owner
performance benefit. Actor-away target absent/required0/coldwork/facts/payload/
shape charges0, historicalcreated/released81/81 but422 normal positiveleases remain.

Procedural limit: deploy held the game lock, but this subsequent test inherited
LOCK_OWNER after that lock returned and did not own the semaphore. Root confirmed
solePID38192 and other agents performed no runtime. Retain actual correctness
evidence; do not label this a lock-controlled performance comparison.

Fresh Reset generation rejects stale publication, but same-world DropStale has
a late post-cancellation PAA publication gap. No generic linearisation or actual
full-worker latch proof is claimed. Properly self-locked geometry diagnostic is
pending; whole plan OPEN/defaultsOFF, no paging/reclaim/performance acceptance.

Properly self-locked geometry003921 now passes clean0 AllocationDiagnosticOnly,
convergenceMeasuredfalse. Report unionV88982216/I14558520/658meshes across189
valid inspected models/658LODs/1253section visits; registry683/visited256/skipped67,
missing0/truncatedtrue. Overlap0 with ownershipCoverageCompletefalse means only no
shared allocation across these inspected rows; it does not certify global
exclusive/reclaimable bytes or selected demand. Global poollive380859292/
cap637534208/retired0 remains broader than the union. Request capped1 registry
record/1row passes bounded reporting (first negative-cache record gives0valid
models), and stale request-ID refusal passes. Controlled alpha retry remains
pending; no repeated-convergence/performance/paging acceptance.

Properly self-locked controlled004139 retry now passes clean0 on unchanged0f.
Cold657 capacityskips/282 factsputs/61 hits/0 miss/cancel,304prepared entries/
536866368 capacity bytes, peakconversion453154616. Same19985/live230706772/
cap318767104/objecttexture1262855596/count1573/backend2744302016 retains coverage.
Actor-away target absent/required0/cold debt0, historicalcreated/released81/81
but422 normal positiveleases remain. This closes the procedural runtime gate;
the initial unprotected003805 remains actual correctness evidence, not controlled
performance. Fraxinus still block-path alpha30.1/model31.2ms, shape14.682ms:
no owner-thread gain, FPS or no-regression acceptance follows. Later PAA
publication-token fix is pending/unbuilt, GPU LOD feedback is a separate proposal;
neither is part of0f runtime. Whole plan OPEN/defaultsOFF.

## Sept27 accbc27e: PAA cancellation, dispatched LOD samples and default screening

All-target319/stamped18 builds pass; combined137747 assertions/206cases
(204PASS/2explicitSKIP), actual paused PAAworker110/1, fresh1MiBTTL.01alpha362/2,
Rustfeedback7/ABI9 pass. Initial opt-in shader fails because target is reserved
WGSL; retained failed log and demand_slot repair validating both variants recorded.
OFF uses exact original borrowed WGSL, no composer/source allocation/new binding
or counter expansion. Optional PAA jobs capture bounded model cancellation tokens.
Put/claims reject invalid tokens, including DropStale after last worker check;
invalid cached payload may linger until bounded sweep. Cancellation after a
successful prior claim is not rollback. Actual paused worker test covers this
boundary; cold011720 PaaCancelled0 is compatibility, not forced runtime cancel.
General immutable source/freshness proof remains limited.

Cold011720 passes clean0: original DayZ83912800 absent→CPUgeometry/fire/view,
deferred/visualfalse/VB0 and real fire contacts. Actor-away target absent,
81created/81released/required0/cold work/facts/payload/shape charges0, but422
normal positive leases remain. GlobalCapacity/watch16384/Readyregions0 persists.
Publication inventory18848B/DDSalias0;318prepared entries/536780472capacityB,
facts285puts60hits0miss/cancel/skips665, peakconversion475294444B.
MaxBank1.8059/MaxTail1.6447ms observations, not hard bounds. Fraxinusalpha22.3/
model23.3ms stillblock path/handoff0. No performance/lifecycle/save claim.

LOD011828 passes clean0 AllocationDiagnosticOnly/convergenceMeasuredfalse.
Epoch1 request/Busy/cancel and epoch2 replacement/stale-ID refusal pass.
Samples12of16attempts/drop4/mapfail0/dropdispatch0/views72/passmask7(main/color/
shadow)/age3. Selected89/59/56/97/136/36/98/34 masks1/3/3/3/1/1/1/3 within
authored LODs. Copies precede reused counter clearing, mapping follows submit
nonblocking. Cached undispatched views not covered; ownershipCoverageComplete/
cachedViewCoverageCompletefalse. Subsetregistry683/visited256/skipped80/valid176,
missing0/truncated,631mesh/1097sections uniqueV75947840/I11674404; inspected
overlap0 does not prove global exclusivity/reclaim. No paging implemented.

Matched disabled baseline004802(0f)→current011529(accbc) clean0 screening:
736→823frames/drop0, median23.8955→23.198, mean48.544704→42.648450,
p95150.296→134.881, p99435.774→421.172ms, but max856.428→1252.678 and
initmax705.389→1240.396 worsen. Percentiles floor((n-1)*p), evenmedian middlepair.
Same route/settings/initial19923payload; final19923settled counts but geometry
730649324→713414164 and textures2835207828/2560→2748831188/2489 differ.
Currentpose5 pending19632 vsbaseline settled19972. Single ordered async/cache/
clock-confounded comparison excludes headline gain/no-regression acceptance.
PBOmodel1/DDC0/budget20000/fixedLOD1/1280x720MSAA4VD2000 matched renderer fixture
is not all normal defaults. Terrain telemetry opt-in attribution only.
FullplanOPEN/defaultsOFF/performanceAcceptedfalse.
Terrain trace011941 passes clean0 with opt-in page/slow-frame clocks. Initial
pre-route page upload715.662/update716.503ms includes ensure696.480ms; outbound
56.597/update56.915 includes ensure47.037ms; return319.058/update319.434 includes
ensure312.189ms. All505layers/504ensure calls;7193descriptor records,1363/1368
candidate materials and20276/20280load calls, scan.252–.305ms, descriptor2.342–
3.164ms, final explicit acquire0;28page evictions per traversal window change.
Return frame647 ground319.783/whole484.858ms corroborates active-path cost.
EnsureUploaded is inclusive owner elapsed with internal reads/decodes/uploads/
windows, not pure CPU work or GPU elapsed. Descriptor rebuilding is not dominant
in this observation. Initial page update precedes route capture and is not a
route tail percentile. Frame532 init829.587 includes residency829.535ms versus
renderer.001ms; next residency814.242ms. Next texture-detail telemetry is pending/
unbuilt, not part of accbc. No terrain optimization, FPS or no-regression verdict.

## Sept27 texture phases and counted/timed admission screening

ec5dfcb1 opt-in texture telemetry builds19/stamp18PASS, installedclean04a5856a.
Counted012836 passes clean0. Threshold2ms/first128coldterrain-familysources is
bounded source order, not top-N or all504Ensure calls; only3qualifying logs,
no truncation. Actual s_017_030 read.848/create76.848/total77.804ms;
m_017_030 .304/1.750/2.742; s_020_0261.341/27.424/28.847. Preparedfalse,
uploadedtrue/fallback0. Create is owner elapsed including producer wait,
not GPU elapsed. Family filter layers/LCA/LCO excludes other source names.
Initial pre-route ensure1104.554 versus descriptor3.907/load61.661ms; return
ensure262.763ms dominates total269.183. The three logs cannot be summed to
explain all inclusive Ensure cost. No behavior change or texture optimization.

Same04a Counted012836→Timed013022 clean0 single ordered screening:
876→975frames; median23.0405→23.143, mean39.757965→34.752372,
p95119.825→89.504, p99356.144→189.474, max1055.883→455.512ms.
Initmax912.984→365.289 but groundmax270.114→287.965 worsens. Loggedwarm
max893.4→269.1ms includes startup, not routepercentiles or16ms guarantee;
actual Timed674-object warmbatch55.5ms remains. Sameinitial19923payload;
bothreturn19923settled, but geometry716552916→732333188 and textures
2776835208/2530→2924549716/2652 differ. EarlyTimed residents15846vs16256,
18129vs19341,16559vs19032, pose5pending18145vs19497 show transientcoverage
tradeoff. Root inspected returned street/bus/buildings/furniture/trees matching
narrowly, minor clouds differ; no moving-route visual acceptance follows.
Counted remains production default. Cache/order/async/clock confounds and
allocation differences exclude Timed default/no-regression/FPS acceptance.

Normal admission source audit: LandSave.cpp2791+ counted mode deliberately
bypasses wall-clock ceilings for warm admissions up to1536 attempt cap; known
model does not guarantee cheap ObjectCreate/registration/texture work.
Existing comments contrasting cheap warm work are not a measured cost bound.
Observed city_school initial cold row01:15:55.083 has texture-read549.8ms versus
alpha83.9ms, total926.4ms; this is before route capture, not the route outlier.
Target texture/owner work separately from alpha. Lazy per-image bindgroup
candidate is in progress and not implemented/runtime-proven in installed04a.
First normal traversal performance fix remains pending; fullplanOPEN/defaultsOFF.

## Sept27 lazy bindings rejected as default, parked refill kept opt-in

58ea13c4 four-Rust lazy single-image bindings pass7 actualGPU tests in1.64s:
firstuse/reuse/replacement/oldretirement-white behavior. 3640a97e adds defaultOFF
bounded parked-refill gates (source/static/ref/lease) and tiny unused-layout rename;
alltarget29/CPP87135 assertions142cases140PASS2SKIP incl4purepark cases pass.

Historical364 lazy-defaultON014820 screening passes clean0 but compared04a
Counted012836 mean39.757965→42.592059ms worsens, swap p9521.497→46.523/max
410.252→680.191 and groundmax270.114→389.362 worsen despite p95119.825→117.480.
Initialpayload and almostallpose counts match; finaltexture/geometry differ.
Returnpageensure262.763→383.324ms worsens. No measured firstnormalhitchfix or
broad no-regression acceptance. Preserve mixed evidence; do not promote default.

Same364 parkedON014959 passes both strict actual positive gates:896deferred
rows/289activation rows, totals8134checks/1451images/0failures/0leaseChangedStale.
Repeated observations/images are not unique model counts or reclaimed bytes.
OFF→ON p95117.480→131.149/max957.644→1213.838ms worsen, initmax856.570→
1103.118. ONpose4pending18014vsOFF19966settled; final geometry691598216vs
713510204, textures2708507220/2443vs2771417940/2505 differ. Budgetpressure0/
pressuregrowth0: this proves leased-slot activation, not pressure handling,
convergence, reclaim, fullsourcefreshness or Nanite. Prototype remainsOFF.

a2c944bc guards Lazy behind WGR_LAZY_TEXTURE_BIND_GROUPS=1 and restores original
eager group creation order with filledOnceLock. Initial compile fails E0425
layout because old_unused_layout rename; retained failure,170fde9f repair/build18
PASS. Default test8listed cases includes7actualGPU/1explicitflagmismatch early
return (not adapter skip); fresh opt-inLazy1actualGPU PASS. Lazy/Parked startup
defaultsOFF/Countedunchanged. Installedclean170015926 passes clean0 compatibility:
877frames median22.989/mean39.331944/p95122.031/p99325.633/max909.544ms;
final19923settled req0 geometry707679832/texture2796844308/2547/cap1275068416.
Ordered cache/async/GPU-clock and differing allocated work exclude peak/no-reg/
performance acceptance. Warm-profile LandSave candidate is in progress,
not installed. FullplanOPEN; normal performance fix/pressure proof pending.

## Sept27 aggregate warm admission attribution

086249d3/a18ead01 opt-in warm diagnostics retain initial failed build (legacy
Log macro collision), repaired19target build/deploy pass. StartupWarmProfileOFF;
Counted/eager/LazyOFF/ParkedOFF unchanged. Actual021434 sole-root/lock clean0
passes the runner's required aggregate-row gate. ProfilingON changes observation
overhead, not admission behavior; not a causal FPS/no-regression comparison.

Aggregate row7 centre100,40/1536warm scopes total455.521ms, geometry0,
scene450.224/register285.696/proxy163.930, meshBuild3.207/sections435.347,
textureRead362.606/create66.752/alpha0. Aggregate row9total185.767/read164.169/
create14.680; row10total133.553/read113.064/create11.299, each1536warm scopes.
Known-model warmth does not imply cheap creation: observed texture read/owner
work dominates these warm batches. Inclusive scene/register/proxy/section/
texture fields overlap; they cannot be summed as disjoint CPU partitions.
First128aggregate updates are independently capped from first128individual
slow>=2ms rows; aggregate counts include every opened scope, including sub2ms.
Source order is not ranking and does not capture whole frame/GPU elapsed.
848frames are screening only; final19923settled/required0, geometry705016536,
textures2734326836/count2478. Unrelated Landscape owner-marker candidate is
uncommitted/unbuilt and not in installeda18. FullplanOPEN/performancefalse.

## Sept27 unrelated marker-light owner allocation gate

82cb4b80 build19/deploy pass; actual021758 passes cleanexit0 on installed
82cb4b80+dirty stamp02:17, exact binary/provenance hashes recorded. Shared
IsA3StaticMarkerLightSource preserves the prior case-insensitive a3-backslash
predicate and skips empty owner-map insertion/erase at admission/promotion for
rejected families. Accepted light construction remains unchanged by source
inspection; DayZ rejected-family route is not an accepted A3 runtime-light proof.

Warm profilingON861frames is diagnostic, not causal FPS/no-regression evidence.
Aggregate row14warm1536total887.291ms includes textureRead757.532/create86.630,
mesh13.880/alpha0/geometry0: read problem remains. Inclusive fields overlap.
No broader performance acceptance or default-policy change follows.
Upcoming PacTexture subphase diagnostics are pending/not included; fullplanOPEN.

## Sept27 compressed mip read subphases

da3a2866 build19/deploy pass; actual022229 clean0 on matchinginstalledda3.
Opt-in active profile scopes include cold, warm and alpha reads; first128>=2ms
source-order cap saturates startup02:22:49.169. No complete moving-route or
alltexture bottleneck distribution can be inferred from this truncated sample.
Betulahumilis7mips: open.005/loop19.675/packedAllocCopy3.324/LZO15.524/raw.012/
total19.681ms (4LZO/3raw); singlealpha mip open.003/loop20.913/packed10.733/
LZO10.176/total20.916ms. Packed bucket includes allocation/copy; these are owner
CPU elapsed, not GPU time. SourceRead buffer zeroing/prepared-store take are
outside Pactext rows; this is not a partition of whole TextureWgpu read time.
Original BC compressed payload/quality/policy unchanged,831frames diagnostic
only/performancefalse. Zero-copy and FileCache-flush source candidates are
being edited and not validated or included. FullplanOPEN/defaultsunchanged.

## Sept27 source-read/cache fixes and rejected LZO match experiment

83845a71 removes selectively flushed bank bytes from FileCache budget before
deleting owning references, matching existing eviction accounting. 768af3a3
borrows bounded contiguous disjoint input spans for immediate PAA mip inflate;
refusal preserves cursor/flags and uses original copying fallback. Span never
outlives backing stream; bounds/overflow/alias guards remain. Combined699target
build passes. Targeted Engine118715assert14cases includesLZO; Core71assert3cases
includesdisjoint-span/file-cache. Engine unmatched Core-only filters and Core
unmatched Engine-only LZO filters are not claimed as executed coverage.

16135e50 LZO distance1/disjoint fastpaths pass correctness but are NOT promoted.
Actual11rawLZO mips from3originalplants.pbo members inflate12517376bytes with
exact bytes/consumed parity. Standalone reported/O2/noLTO,3warm rounds and24
alternating interleaved rounds; source includes both inline decoders in one TU.
Initial median19.4517→20.0269ms(+2.96%), max25.4199→24.2003; Threshold64
median21.4791→21.6082/max24.7203→26.1522; FillOnly18.7603→18.9652/
max25.5651→25.4946. Batchmax is not framep95; warmed CPUcomponent excludes
I/O/GPU/FPS. No variant demonstrates accepted benefit. e0e3c2cd restores exact
original production decoder before deployment; frozen reference/3differential
tests retired to private benchmark artifact, independent long recurrence test
retained [lzo-recurrence]. Hashes preserve corpus/provenance/results/source/exe;
current source does not certify every historical variant's compiler inputs.

Original-decoder rebuild and actualruntime are pending at this checkpoint;
do not infer restored tree installed from preceding candidate tests. FileCache/
zero-copy changes retained pending runtime. FullplanOPEN/performancefalse.
Final restored-original decoder build44/deploy now pass, matchingcleane0e3c2cd
stamp02:37. OriginalEngine51658assert11cases/Core71assert3cases pass/noSKIP,
with actualbinary tag scopes distinct. Actual zero-copy023702 clean0 confirms
positive borrowedLzoMips: betulahumilis7selected/4LZO/borrow4/3raw mips open.004/
loop11.937/rangeacquisition.001/LZO11.923/raw.007/total11.941ms. Historical
packedAllocCopy label now covers range acquisition or original copy fallback,
not necessarily allocation. First128 bounded startup/activeprofile rows do not
cover all moving-route reads. Original decoder remains exact; compressed payload/
quality unchanged. Aggregatewarmrow20 1536total958.266/read838.632/create62.582/
264uploads alpha/geometry0 confirms remaining hitches. ProfilingON879frames
is compatibility/attribution, not FPS/no-regression acceptance. At this historical checkpoint the profileOFF samebinary screening was pending. FullplanOPEN/defaultsunchanged.
Profile-OFF023850 subsequently passed clean exit0 on the same clean e0 binary,
with stable executable/DLL hashes, Warm/Terrain/Lazy/Parked OFF and Counted.
912frames mean37.439431/median23.0675/p95111.124/p99303.084/max1087.966ms
versus170 eager-default877frames39.331944/22.989/122.031/325.633/909.544ms.
Lower mean/tails but worse maximum is mixed ordered screening, with renderer/
source/cache/admission and allocated-work confounds; not FPS/no-regression
acceptance. Returned19923/required0, geometry706778992B/textures2729695956B
(2451), different from170. Default zero-copy is now installed; original decoder
retained. Root compared before-da3 and after-e0 profile-ON settled screenshots:
same local bus/street/buildings/foliage/textures, not whole-route quality or A3
compatibility. FullplanOPEN/performancefalse; future persistent GPU owner ledger
source is not installed or verified by these runs.
Persistent owner attribution395a3cfb/10d3206b is startup opt-in only; all-target
250/review19 builds and123assertions/8cases pass. Clean10 stamp actual025802
passes AllocationDiagnosticOnly, zero traversal cycles. Complete means only
producer/model/CPU-owner facts from first create at a drained event cut;
ownershipCoverageComplete false/reclaim0. Actual2602allocation/2506borrower
records,2445liveCPU/1814modelLODlinks/61cancel creates, epoch1/reasons0/
mailbox0/drops0. Estimated vertex331583572+index49275720=380859292 equals
sampled pool-live here only, not general allocation proof. CPUreferenced
379148808/retainedshared278823860/standalone100324948 bytes. Inspected overlap0
despite2445CPU borrowers proves subset overlap cannot establish exclusivity.
Caps8192allocations/32768borrowers/32768links/mail4096 and32768per-register
LOD+section visits fail incomplete; retired links/failing allocation sticky.
Retired metadata retained until reset; destructor numeric detach is not actual
uploader/GPU retirement acknowledgement, cached/in-flight coverage incomplete.
Default ordinary buffer allocation/subclass unchanged and no extra buffer heap
OFF. Default OFF runtime was pending at this checkpoint; no FPS/MP/reclaim/paging/performance acceptance.
FullplanOPEN.
Same10 default-OFF025919 subsequently passes clean exit0: persistent enabledfalse/completefalse/CPU0/trackedalloc0/reclaim0, while ordinary bounded geometry report remains positive. This validates default flag gating only, not convergence, physical GPU acknowledgement or performance.
PAA LZO replay0757a71d/4bc427cc build22/final19 and51750assertions/15cases
productionflag1/noSKIP pass; clean4bc installed03:19. Both actual ON031919/
OFF032059 clean0 pass, but experiment remains defaultOFF and NOT promoted.
ON689frames mean52.964115/median22.998/p95205.144/p99612.148/max1250.195;
OFF856 mean40.237220/median23.2005/p95127.035/p99341.288/max902.483ms.
This pair uses nearest-rank ceil(n*p)-1 quantiles, unlike historical floor-index screens; even-count median averages the middle pair (OFF23.2005).
Returned19923/required0 both; ON geometry709821528/textures2638213236/2387
versusOFF711843332/2759971636/2502 and transient coverage differs. Worse ON
mean/tails/lag supports no promotion, not a causal FPS/no-regression verdict.
Bounded first128 startup replay observations saturated03:19:42: last146hits/
2278misses/75694080hit-output bytes/31459397store/33554040peak/41entries/
2237evictions/0oversize/failure. Not full-route counters; LZO timer includes
cache comparison/copy/admission. Exact current original compressed input bytes,
fullmemcmp and outputcapacity protect memoized result, not source/header freshness.
Fixed32MiB logical charged data excludes allocator/RSS/caller buffers. Original
decoder and compressed content unchanged. Subsequent owner-probe source is not
built/installed here. FullplanOPEN/performancefalse.
Renderer mesh-record facts f88379f3 build247/Engine8373assertions10cases/
Rust4PASS0ignored342filtered pass; cleanf88303:35 pair ON033510/OFF033623
clean0 AllocationDiagnosticOnly zero routecycles. ON boundedcomplete2602handles
inspected2472present130absent,130scheduled-retired allabsent/0present-retired,
0absent-with-persistent-owner/unknown/invalid/duplicates/truncation,epoch1/calls1.
Exact source pool spans vertex331583572/index49275720 sum380859292 match this
live-pool cut only, not universal allocation proof. Persistent producer/model/
CPU scope complete with2445CPU borrowers; globalownership false/reclaim0.
Absence means generational renderer record removed, not queue completion,
device-memory release or allocation reuse safety. Snapshot hard8192; no actual
all-pass/cached/in-flight ownership proof. OFF enabledfalse/calls0/requested0/
completefalse/bytes0 while normal allocation report works; no default queries.
Root pre/post pair installed hashes match EXE02238BE88F87011E36FDB7A737CE67257D10D7AEFDCFF12C4BE0E62088D7D224
and DLL05D8460B15E3ECFF8B5013CAD4AA36DB83A71F4679C2A4CF2EBC572DBAB9C6E9.
No FPS/MP/reclaim/eviction/paging/performance acceptance. Future sample-fingerprint
prototype is unbuilt/not included. FullplanOPEN.
Bounded replay fingerprint e966bb56 build20/test303assertions7casesflag1/noSKIP
pass, clean installed03:42. Original full-byte equality still guards sampled
fingerprint collisions, no freshness theorem. Component11unchanged PAA payloads/
12517376decoded bytes,3warm+24rotating4arms,sameTU/O2/noLTO/fulloutputcheck
outside timedbatch: fullFNVmiss median34.5138/max40.4774 versus samplemiss
23.8338/29.5811; fullFNVhit6.796/7.2309 versus samplehit1.28785/1.4431ms.
Actual baseline filename PaaLzoReplayCacheBaseline.hpp; manifests/hash provenance
preserved. This reduces local hash cost only, not FPS. Actual ON034235/OFF034452
both clean0 pass: ON777 mean45.151363/median22.946/p95145.379/p99525.277/
max896.143 versusOFF89238.894854/23.1105/126.091/384.556/968.986ms, nearest-rank
ceil(n*p)-1/evenmedianmiddlepair. Returned19923/required0, butON geometry729493612/
textures2823455764/2547 versusOFF704386448/2817071860/2527 and transientcoverage
differ: not matchedwork/no causalbenefit/no broadregressionproof. Startupcapped
last146hits2250miss72548352hitout28573820store33552520peak23entries2227evicts0
failure/oversize, not fullroute. Actual candidate unaccepted/cacheOFF/unpromoted;
no endless repeat-screening. Future<=256 simulation audit unbuilt/not included.
FullplanOPEN/performancefalse.
Small-inventory Ready audit7e23999c clean03:52 build21/tests26436assertions26cases
noSKIP pass. Every<=256 fresh query re-audits referenced actual geometry via
AuditRegisteredGeometryNow before Ready and after constructors, one shared2ms
whole deadline across beginning/final/watch scans. No per-model reset; combined
cost may remain Pending even when one scan fits. Three new unit cases use actual
adapted shapes with synthetically labelled auditedODOL IR, NOT retail ODOLfile
decoding. Unversioned SetPos/SetMinMax/rawClip/offbin/undersizedparent refusals
Unknown preserve collision sentinel, shared deadline returns Pending.
Actual gameON035221/headlessON035310/defaultgame035411/headlessdefault035424
all clean0 pass. Game cold logical2/resident0/required0 queryReadyReady, existing
transaction/lease/move/reacquire/Fire checks pass; mutation and saved-survivor
remain Unknown. Headless rendererNone Ready2regions/2leases/realwallcontact.
Defaults Disabled zero created/leased/positive, None headless; no new AI/default
enable. Narrow fixture only, no FPS/broadMP/retailDayZ/ODOLgroundproof. DayZ2352
models/watch16384Capacity remains. Cached GPU ownership research excluded.
FullplanOPEN/performancefalse.
PAA BC boundsdb7579b6/EOFrepair7b92dd29 build22/final20 pass. First unit7cases
6pass/1fail (581/582assert) assumed truncated.fail(), but byte-get sets EOFonly;
retained failed log, repaired final583assertions7cases/noSKIP pass. Fresh mip
metadata bounds BC capacity; header fail/eof checked, raw<=capacity keeps short/
zero tails, valid LZO mismatched source refused. Raw converter only exact full
4x4DXT1->1555; complete EOS nulloutput retained/truncated refused. Metadata/
layout canary proof is not arbitrary caller buffer-capacity proof.
Clean installed7b92 at04:17, actual041703 clean0/defaults+profiling+replayOFF
pass.821frames mean41.998479/median23.361/p95128.245/p99437.288/max980.730ms,
nearest-rankceil(n*p)-1. Returned19923/required0/geometry713550680/textures
2749224436/2488. Root compared settled with earlier fingerprintOFF034452:
same localPOI/no obvious geometry-material regression, not wholemap/performance
acceptance. Direct deploy refused missingLOCK_OWNER before copy, subsequent
proper locked deploy passed; no mismatchedgame run. Actual provenance binary
hashes EXE04BF3C6A43D010B1F5A84A32BF5088734F70458E78F4FD2C2DAEACBFD8F4C43D,
DLL39A04A78A496930022AB77796C2962A0F6F060B6AAA5B2B36914FD2845AC15E0.
Initialized BC-read API candidate unbuilt/caller unwritten/not included.
FullplanOPEN/performancefalse/no causalno-regression claim.
Initialized BC destination experiment is REJECTED UNBUILT/UNTESTED. Standalone
originalLZO11actualplant PAA payloads12517376decoded bytes:3warm+31alternating
rounds fresh per-mip vectorzero versus new[]uninit, exact fulloutputequality
outside timer. Zero median21.877/max28.3649 versus uninit22.0143/28.3841ms,
about0.6%slower and no material gain; no VFS/chainAPI/upload/FPS/causalregression
proof. API/caller/test candidate independently reviewed but NEVER built/tested,
all restored/removed. Private ignored initialized-bc-unbuilt-prototype.patch and
test_paa_initialized_bc_chain.unbuilt.cpp plus benchmark provenance retained.
No flag/API shipped, installedclean7b92 unchanged. Reject complexity without
measured gain; normaldefaults unchanged/fullplanOPEN/performancefalse.
MeshACK9996a122/PAAattemptdiage54efc50 build475all5targets pass; Rust8PASS/
0ignored346filtered; Engine9277assertions30cases28PASS2SKIP (4special1MiB/TTL.01
skip sections), PBOtex+diag flagsON, notallcasespassed. Clean e54 installed04:43.
Actual meshON044319/default044724 and PAAON044504/default044812 all clean0PASS.
Mesh DiagnosticOnly0cycles Queued->next main completion Ack3:2602handles2472
present130absent,poolgen10/sourcebytes380859292/capacity637534208/retired0.
Ticket1relativeordinal1/fullreport; ticket2ordinal17/Model1Row1 truncatedtrue,
staleoldrequestInvalid. Relative ordinal begins diagnosticstate allocation,
not globalframe/wgpuSubmissionIndex. Completion proves preceding queue work,
not future cached/in-flight references/devicefree/reclaim. Max3 includes
cancelledcallbacks acrossreset,8192handles/batch; droppedcallback remains
Submitted honestly, device-loss timeout possible. OFF enabledfalse/ticket0/
state0/factsCalls0/ownerOFF. No synchronousGPUwait/defaultqueries.
PAAON915frames/default867frames nearest-rank route numbers are retained only
for screening, not throughput acceptance: continuing clocks/filename scans after
64logcap and differingPBOtexture modes confound. Prefix64 conversions434inputs/
150preSkipped284starts274Putaccepted10rejected10filenamewhole-attemptoverlaps;
workerSUM1151.309successfulreaddecode,84.603rejected/130.941includingfacts ms,
maxconversionterminal185.529,0overflow/readfail/cancel/throw. Cannot pair individual
overlap/rejection/Putreason; notroutewall time. GeometrydiagPBOmodel0 versus
PAAmodel1 are different workloads. Root ON settled localPOI showed no obvious
new material/geometry regression; root also viewed OFF044812 against ON044504 and older7b92041703: same localPOI/no obvious change, not whole-world/no-regression acceptance.
No default activation/eviction/paging/FPS/performance acceptance. Future member-
bound in-flight candidate unbuilt/untested/excluded. FullplanOPEN.
Exact-member PAA suppressionae96556a build22all5targets/test679assertions13cases
noSKIP pass, cleaninstalled05:06. ON050709/OFF050847 both clean0PASS. First
observed suppression1/active1/currentmetadata21760/peak41184 bytes,0overflow/
unsupported/contended; not route totals/RSS or guarantee peerPut publication.
Ordinary owner fallback remains. ON911frames mean39.084184/median23.831/
p9592.940/p99319.286/max1010.128; OFF92239.297031/24.3535/85.771/401.291/
1364.731ms nearest-rankceil(n*p)-1/usualmedian. ONgeometry709881720/textures
2745904404/2487 versusOFF709824720/2745860724/2486; orderedcache/asyncwork/
diagnosticcost not matched causal/performance acceptance. Root viewed both
settled samePOI no obvious new geometry/material change, localonly. Suppression
OFFdefault, fullplanOPEN/performancefalse. AllflagsOFF normalcontrol was pending at this checkpoint;
newposecache source unbuilt/not included.
Sameae allflagsOFF051036 subsequently PASSclean0, stable installed hashes.871frames mean39.697796/median23.514/p95131.467/p99333.420/max942.080ms nearest-rank; geometry712369276/textures2749617844/2494. PBOtex/attemptdiag/inflight false, no new default work; compatibility only, not no-regression/performance acceptance. No posecaller prototype installed; fullplanOPEN.
Local CPU pose witness5bd76495 cleaninstalled05:25 build18all5targets/Rust7PASS
0failed0ignored354filtered. ExactflagOFF; bounded64blocks/512KiBlogical/64audits/
1024observations, finiteexactbytes/ORmask/invalidnextframeNone/resetOFFforce;
trace128rows(two types), no OFF tracing. ON052545/OFF052658 clean0PASS. Actual
startup matrix mutation mesh4294968130/palette1/index18+12/legacyworldContentKey
15079445822454082016/epoch100843/lightDirty0 revision1->2/sourceplans2->3,
refresh14/mask16383/no unvalidated witness. Startup observation is NOT script
commandtransition causality or exact unhashedworld/all-view contracts. Later
plans everyposechange10casterobservations/14rendered0cached versusOFF rendered0/
cached14/posefalse/observation0: frequent work, no performance acceptance.
ON949frames mean2.165123/median1.546/p952.104/p992.755/max166.702 vsOFF951
1.979384/1.466/2.055/2.465/152.108ms nearest-rank. Dark ON/OFF images root viewed
no obvious missing assets, cannot prove a visible stale-shadow fix; no visual
acceptance or default promotion. Retained/sky/concurrentmeshwrite coverage open.
Default DayZ control was pending at this checkpoint; cargo source-only proposal blocked by modernloader/
dedicatedpurpose/lease lifecycle, no unused flag shipped. FullplanOPEN.
Same5bd defaultDayZ052839 subsequently PASSclean0/stablehashes,835frames mean41.499448/median23.189/p95142.363/p99391.592/max1002.120ms nearest-rank; returned19923/required0/geometry711818480/textures2762855092/2501. No new normal pose/trace/experimental flags; differing work/ordered caches not no-regression proof. Root viewed settled5bd versus previousae default same localPOI with no obvious new geometry/material coverage changes; narrowlocal only, not wholemap/no-regression visual proof/fullplanOPEN.

## Sept27 registered-model census and warm registration quota

3cd04d25 builds all five targets in249steps. Engine114assertions/6cases pass
without skips for registration-cost/preparer streaming-budget; Rust current
registered-mesh references3pass/0ignored/361filtered. Cleanmatching3cd installation
06:07: EXE AF7C52709C40692C27E60F97A20E6A35E53BD0D601B3F1AF3E83702C5DF23452,
DLL FAA5C45CE9CFBD8F71C51EDA1F1090693A7D0C7F2567103E2893D9D045DB38D3.
The census inspects bounded current registered model/LOD/section references only;
it does not certify selected draws, CPU upload owners, GPU in-flight lifetimes,
exclusive allocations or reclaimable bytes. Actual censusON061057 PASS/clean0 AllocationDiagnosticOnly, convergenceMeasuredfalse:
2602handle rows/490currentmodels/1814LODs/3272section visits; referenced meshes,
model references and modelLOD references each1814, section occurrences3272.
Calls1/validcomplete/missing0/refusal0,8bounded full-generation string samples
with flags0. Globalownershipfalse/persistentReclaim0 remain unchanged.
CensusOFF061233 PASS/clean0: enabledfalse/calls0/rows0/empty samples;
ownershipCoveragefalse/reclaim0. Same3cd allflagsOFF061347 PASS/clean0,
866frames mean40.073848/median23.7215/p95123.804/p99356.248/max903.406ms,
nearest-rank/usualmedian, dropped0; returned19923/required0/geometry713270272/
textures2773165460/2497. Quota/warm profile/othernewflagsOFF; root settleddefault
againstON/OFF has no obvious local coverage/material change, not wholemap/
no-regression acceptance.

Samebinary quotaON060747 and OFF060925 both PASS/clean0. ON904frames
mean37.955302/median22.915/p95110.707/p99277.355/max538.946ms; OFF867frames
39.625609/23.267/125.632/362.199/1069.472ms, nearest-rank ceil(n*p)-1 and usual
median. Sampled classification/limit assertions pass, with actual warmCharged2
at limit2. This bounds counted registration attempts, not a frame deadline,
texture readiness or total route work. Transient coverage is slower ON: pose0
15382 versus17279 OFF, pose1 18021 versus19970. Both return19923/required0,
but geometry739486152 versus703680528 and textures2928044692/2654 versus
2736074548/2482 differ. Ordered profiled cache/allocated-work confounds and
transient coverage prevent performance/no-regression acceptance or promotion.
Root viewed settledON/OFF at the same localPOI without obvious coverage changes;
this is not all-route/full-map visual proof. New flags remain OFF/fullplanOPEN.

The next proposed StageD slice is bounded warm-only primary-PAA preparation on
existing workers/store; current cachedShape prefetch bypass leaves measured
synchronous texture reads. This is a gated recommendation, not implementation:
implementation requires missing actual physical-member origin on texture source headers;
current owner capture has zero certified eligible bindings. Agents are investigating
actual-init source provenance first (unbuilt/notincluded, not an external blocker);
no new jobs shipped. Thereafter require
immutable source leases, desired-generation cancellation, independent
texture-only jobs without model-state takeover, bounded owner capture/worker
fairness, fallback for uncovered roles and equal-work transient coverage/performance.

## Sept27 initialized-PAA actual source-binding prerequisite

267d590b all5targets704steps PASS; separate freshflag EngineOFF30assertions/
2cases and ON326/4 PASS/noSKIP. CompatibilityEngine401/6 passes owned-read/
PAA bounds; qstream/policy is unmatched there and not covered by that result.
Actual currentbuild Core qstream/policy33/6 PASS/noSKIP separately. Installed
clean26706:33 EXE DC331870863D6FD61D0FA2788B9E01FF208A3390B53CBB68F8167189C23E0789,
DLL DDDEE19B75F816214FB359A845A059E6120C9950B4B8649368D5E45BC94E643A.

SourcebindingsON063332 PASS/clean0: mapped raw PAA/init header capture retains
256live bindings/117728known C++ bytes, caps256/2MiB; wrappedReads/initRetains256,
captureRefused0/capacityRefused6107/allocationRefused0. This proves actual
source capture only, not warm workers/upload readiness/work moved. Logical
metadata charge excludes kernel/allocator/RSS. This267 still opens temporary
fresh leases after the cap before Create refusal; saturated fast-refusal and
CurrentMatchesMountedMember repairs are next unbuilt source, not included.
OFF performs no source capture/leases/wrapper allocations/jobs, but the null
shared branch and16-byte source field remain: not a literal zerofootprint claim.

ON820frames mean42.384855/median23.446/p95139.518/p99345.758/max922.284ms,
nearest-rank/usualmedian; returned19923/required0/geometry711824472/textures
2763204596/2503. Root viewed settledON compatible at the localPOI without obvious
missing assets; no wholemap/no-regression/performance acceptance. Same267 normal
allflagsOFF063544 PASS/clean0:812frames mean43.711812/median23.0455/p95128.351/
p99351.030/max930.685ms, dropped0, returned19923/required0/geometry713843212/
textures2778451700/2513. Sourceenabledfalse/live/known/wrapped/init/capture/
capacity/allocation counters0, othernewflagsOFF. Root viewed OFFversusON settled
without obvious local geometry/material difference. Unequal allocated work/ordered
cache and mixed ONmean42.38 versusOFF43.71, ONp95139.518 versusOFF128.351
prevent performance/no-regression acceptance. No warm jobs or speed feature
shipped; actual-init provenance is a prerequisite in progress, not an external
blocker. WholeplanOPEN/defaultReady/performanceAcceptedfalse/newflagOFF.

## Sept27 bounded warm-only texture jobs: actual positive gate FAILED

4db08e8f locked all5target build613scheduledsteps PASS (last shown610). Fresh
OFF76assertions/5cases, ON472/8, fourworker17/1, compatibility470/9, Core33/6
all PASS/noSKIP. Installedclean4db07:10 EXE B0D48FD16F591D9B4095C79614637AC6AE2A6BC7B32ED76AE3C99A3B18CF44E7,
DLL 1F297F1E95537DBED7E480449ECFA1894AEED9BC921C1AE5F74F29F8B2936A07.
Source adds independent texture-only jobs on existing workers, no model-state
parse/adapt takeover. Optional bounds8jobs/64members/2active, conservative2MiB
metadata and64MiB+64KiB scratch peractive undercombineddebt; recentre32models/
256shared capture visits. Reviewed allocation-free DropStale, generation/token
cancellation, actual initialized/current mounted-member source checks and owner
Take/upload path remain experimental/defaultOFF; passing unit tests do not
prove useful production work occurred.

ActualON+WarmProfile071021 strict gate FAILED/runnerexit1: submitted/completed/
prepared/Take/uploads0 across all11poses; no queue/active/scratch/livepayload,
optional queue state1048bytes, returned19923/required0. Cause pending bounded
capture diagnosis. Engine/provenance/harness/samples/jobproof retained; early
strict gate wrote no result/rawframe/screenshot/sourceproof, and none is
invented. No PASS/no-regression/performance gain or default promotion. Bounded
capture log/failed-evidence runner repairs and simulationexpiry work are NEXT
unbuilt source, excluded from4db. No cap raises or unused-feature success claim.

NormalallOFF071247 PASS/clean0/dropped0,873frames mean39.740997,
median23.136/p95123.028/p99380.234/max1019.781ms; nearest-rank/usualmedian.
Returned19923/required0, allwarmfields0/noqueue storage/sourceOFF0. Root viewed
settledPOI without obvious new local geometry/material gaps, narrow default
compatibility only. WholeplanOPEN/defaultReady/performanceAcceptedfalse/allnewflagsOFF.

## Sept27 actual-slot actor expiry and retained warm-capture failure

7326a1a0 all5target22step build PASS; simulation26714assertions/28cases PASS/noSKIP.
Matchedclean732607:20 EXE 8ED3678C58F9DBEB3C5A6CA70F062B172A1F74E793FE021E85F255E8B5AE6750,
DLL 4D218F97F3C707FB8C19E2FD1CDB2134795D0EC1D60D0765510C763A20876BA2.
Automaticleaseclock follows actualslots, manualchannels retained; bounded4
weakdeleted/emptyautomatic cleanup. Incomplete actorregistry membership remains,
service defaultsOFF. Sparse64actor productionpredicate has unit coverage only.
Smallgame072020 PASS/clean0: Ready2wall leases, independentquery roles/move/
reacquire/Fire/damage and weakactor deletion release0leases/queryreferences,
savedwall preserved/required0. Headless072112 PASS/clean0 automatic2wallsReady,
CPUgeometry/Firecontact1001, rendererNone/VB0/required0; its runner does not
exercise headlessdeparture, which is not claimed. No general MP/asset proof.

Warmcapture072133 FAILEDstrictpositivegate/resultpassedfalse/runnerexit1;
ownedgame cleanexit0. Allwarmjobs0/queue/scratch0/optionalstate1048;875frames
mean39.893674/median22.85/p95115.770/p99379.062/max949.280ms, dropped0,
nearest-rank/usualmedian.64loggedcapture calls are a boundedprefix:5have positive
noCurrentSource,2are workcapped. These distinguish missing witnesses but do not
prove global cause or individual physical-source rejection reasons. Actualsource
256live/117728knownCppbytes/caprefused6310. Runner now retains real result/rawframes/
sourceproof/image on strictgate failure; root viewed returnedsettled against4dbdefault
localstreet/trees/buildings/bus without obvious new geometry/material gap. Narrow
stillimage only, no traversal/performance acceptance/noPASS.
Next model-owner capture-purpose source and storefixture are unbuilt/pending,
excluded. No cap raises, usefulwarmwork/gain/defaultpromotion/performanceacceptance.
WholeplanOPEN/defaultReadyfalse/newflagsOFF.

## Sept27 model-purpose witnesses: one actual warm upload, no promotion

06c312eb all5targets28stepsPASS; OFF78assertions/5cases, source-only317/3,
ON203/7, fourworkers17/1, compatibility470/9, Core33/6 allPASS/noSKIP.
Cleanmatchinginstall07:30 EXE 5F9032D4F7FEE12FFB9E1B5DB638FAC724EE0522A95F7ED1E4E7E22FADC1BA12,
DLL 952CBA176210BA2539EFF9080E90DEFCA055B5C437BD5DC834D31E774AEEF4FC.
Actual model-purposeON073015 PASS/clean0/dropped0:3jobs submitted/completed,
6members/puts,1source-matchedTake/Upload1398016bytes, failed/refused0. Finaljob
queue/active/metadata/scratch0, but optionalqueue1048bytes remains; peakmetadata
64392/scratch134348800. PreparedStore still1entry1573776bytes: not whole resource
convergence0. Source256live/132864knownCppbytes, caprefused856/init-wrapped256.
Returned19923/required0, simulationDisabled/positivework0. This proves a small
acceptedupload only, not complete model/proxy/material/readiness/coverage or
absence of starvation. Next faircapturecursor source is unbuilt/excluded.

ON858frames mean40.093906/median23.122/p95121.468/p99462.920/max985.382ms;
source-onlyjobsOFF+WarmProfile073223 PASS841frames42.428484/23.398/144.498/
385.706/1135.439; normalallOFF073347 PASS867frames39.113118/23.256/115.352/
341.659/992.337; all clean0/dropped0, nearest-rank/usualmedian. JobsOFF has
allwarmfields/queuestorage0, defaultOFF also source0. Allreturn19923/required0,
but ONgeometry710374528/textures2745729396/2485, jobsOFF711180832/2759884500/
2503, default701766356/2672071604/2413 differ. Crucially source-only/jobs0 uses
BROADcapture whereas ON uses model-purpose: not an isolated same-sourcepurpose
cost/control. Different gating/work/order/cache prevents causal acceptance;
ONp99 worse than both controls. Root viewed all3settled localPOI without obvious
new gaps; stillimage not all-route/fullmap/no-regression proof. Feature remains
OFF/unpromoted, wholeplanOPEN/defaultReady/performanceAcceptedfalse.

Default model-primary calls retain a cheap disabled RAII/static-flag branch and
numeric thread_local depth4bytes/thread evenOFF. No capture lease/wrapper/job
allocation occursOFF; this is not a literal zerooverhead claim.

## Sept27 bounded warm model capture window

7e46b97d all5build202stepsPASS; OFF973assertions/7cases and ON1065/9 PASS/noSKIP
cover policy/realqueue/cursor/thread/reset and colddefaultpreparerbudget. Landscape
EOL-only indexrefresh has no semanticdiff; clean7e install07:43 EXE 5E2C484A3CC3FC5248898CBA7135C2D8C2E0B32B008D9559A8A270C5E70A2DE3,
DLL DBB76DDB59766E94792F83E1DF1BC8F1E03BAB61EB92FBC2F7849F74E3162251.
OptionalWarmState ownercursor resetsworldgeneration; twofixed32disjointtemps
retain at most64borrowedpairs/32ACTUALcapturecalls under shared256existingwalk.
Advancesactualempty/throw, no coldreorder/additionalShapesFind/worldscan. This
provides model opportunities only, not stationary/bindinglevel/changing-order
fairness. Next manualFireheartbeat source remains unbuilt/excluded.

ON074358 PASS/clean0/drop0,772frames mean44.546067/median23.0945/p95138.911/
p99457.419/max789.056ms;4completedjobs/7members/2Take-Upload2796032bytes.
Finaljobmeta/scratch/queueactive0, optionalqueue1056 remains; peakmetadata64416/
scratch134348800, PreparedStore1entry1573776 remains, not whole convergence0.
Source256live/133024knownCpp/init-wrapped256/caprefused842. Returned19923/required0,
geometry730601736/textures2871337716/2617. Earlier06sameflagprefix3jobs/6members/
1upload differs in orderedasync work/allocations: no causal improvement claim.
Default074532 PASS/clean0/drop0,822frames43.307007/24.206/136.179/368.024/
1288.876ms, returned19923/required0/geometry710528064/textures2749399188/2489,
allwarm/sourceOFF0/queuestorage0. Quantiles nearest-rank/usualmedian. Defaultmax
1288.876 exceeds historical06default992.337; mixedtiming/workloads prevent
acceptance. Root viewed both settledlocalPOI without obvious new gaps, not
all-route/fullmap/no-regression proof. Feature remainsOFF/unpromoted,
wholeplanOPEN/defaultReady/performanceAcceptedfalse.

## Sept27 manual Fire heartbeat expiry

ce403313 all5target22scheduledstep build PASS;26769assertions/30cases PASS/noSKIP.
Cleanmatchinginstall08:06 EXE 23E89C2A57EED3D47343C25A1902150F63974667B88A9482C9D922D8460791C0,
DLL 9D6078C70F41E839E014E8DFE4B731790D88B7C2B618A25BB080E6DD6FE12685.
ManualFire heartbeat logically expires after2seconds; polls do not renew it.
Physicalretirement boundedmax4 withsoft2ms is not a hardphysical deadline;
current <=256model producer is not cancelled. Service remainsOFF bydefault.

Actualgame080638 PASS/clean0: expiredqueryRegions0/statusPending/collidePending
with contacts0, while automatic2wallReady/leased2/borrowedModels1/entries2 remain
intact. RequeueReady/exactFirewall1001 once, fullmove/reacquire/damage/weakdelete
lifecycle passes/required0. Default080730 PASS/clean0 Disabled/no newownerwork/
required0/originalbehavior. Tinyfixture only, no gameplayconsumer migration,
generalretailDayZ/MP/performance/no-regression acceptance. Nextcargo consumer
is unbuilt/excluded. WholeplanOPEN/defaultReady/performanceAcceptedfalse/serviceOFF.

## Sept27 shared Fire consumer quota and AI cargo Ready contact slice

67b5c113/e6082640 all5target62scheduled buildPASS;27340assertions/48cases
PASS/noSKIP. Cleanmatching e60808:17 EXE A82227626EA7C5470BE535BCABEB8072748445B152250CBCC696AC2D622C6554,
DLL 7EF75175194B63D44C9C45C447F427D7FE8FC32F4A9B7D0C39AB20B2E371AE1B.
Sharedquota4attempts/cumulative2ms perjoinedPump includescapacityreset and
lexicalallreturns/exceptions; exhaustedPending performs no Tryvalidation/trace.
Queueproducerwork is NOTcharged; fairness/hardovershoot remain unproven.
Only AIcargo pivot/muzzle gains Ready staticcontacts; allnonReady preserve exact
legacyfallback, leaving unloadedobstruction gap OPEN/read-onlyprobesdefaultfalse.

NaturalgameON081805 PASS/clean0:36passivestate samples, ready9/maxquery1/
collisioncalls2/noWorkRefusal/shots2/ammo28/damage.814349. No harnessQueue/
collision/probe-created interest. CPU2fixturewalls far1200 versusJeep9700:
clearfiringconsumer proof, not newlystreamedbarrel obstruction. OFF/defaultmodern
world081919 PASS/clean0/shots1/ammo29/damage.523728; its runner does not query
stream_simulation_residency counters, so no measuredzeroownerwork is claimed.
ControlledOFFflags/matchingpair prove clearlegacygameplay only. Randomness/asynctime preclude
behavioral/performanceA/B claim. Root viewedONorderedfire localjeep/seatpresent,
notfullvisualproof. Generated256sideoriginalselfcheck/standard32selfcheckpass.
Actualsmall2wallservice+heartbeat/sharedquota082057 PASS/clean0: Readywall1001/
expiry/requeue/independentautomatic/move/damage/delete. No broad originalworld/
retailDayZ/MP/performance/no-regression acceptance. Nextretainedmeshmultiborrower
repair is unbuilt/excluded; serviceOFF/fullplanOPEN/defaultReady-performancefalse.

## Sept27 default shared-retained mesh borrower lifetime correction

29a87291/7f5a0f48 initialbuild FAILED NamespaceIsMainThread; repairall5targets
201scheduledPASS,8561assertions/17casesPASS/noSKIP. Clean7f08:44 EXE 0AE85C02C56F18D84E59020635C7E5316FF64A3F93F503339982CC2222882C54,
DLL 0B01D98BEBC3FADC1C0F583C5AD19C68BE10881FFBE0D772F6BFC13548289556.
Defaultsharing nowtickets shareableproducerbirth beforeenqueue, dedicatedshort
mapmutex/noDrawlocks/CPUlookup only; missingbinding retains originalstandalone.
ConstructionRollback/dead/exception/allqueuedborrowers covered; epochDispatchClose
excludeslateenqueue. Perproducerticket/CPUslot metadata added, notzerofootprint
or physicalreclaim guarantee. Finalborrowerscheduling gap remainsOPEN/nextunbuilt.

Actualprivatefixture084457 PASS/clean0 twoorders newestfirst/producerfirst:
CreateVB2borrowers/helpercount2, normalCPUdrain/RustfullhandlePresent withlast
borrower, lastreleaseAbsent/0. Handles4294967357/12884901949 reuseRustslot with
newgeneration; eachknownpayload21744bytes/cap1MiB. ServiceDisabled/pumps0/required0,
source1001visual retained. Fixture1 optionalstate noneOFF. This proves privatecopy
recordlifetime, NOTpixelDraw/worldmodelunregister/fullownership/queueACK/devicefree.

ActualDayZBEFOREe608082335 PASS/clean0/drop0,854frames mean41.337537/median23.164/
p95124.203/p99400.124/max1208.930ms; AFTER7f084558 PASS866frames40.350622/22.784/
130.902/389.819/984.856, nearest-rank/usualmedian. Bothreturn19923/required0,
geometry710542040→711520920/cap1275068416 unchanged; textures2749355508→2763554036,
private9614004224→9642450944/workingset3833556992→4304031744. P95worse5.4%,
othermetrics improve, but orderedunequalmaterialbytes/cache/asyncwork prevent
causalFPS/no-regression acceptance. RootbothreturnedlocalPOI bus/buildings/trees/
street compatible, notall-routeproof. Defaultsharingcorrectnessfixinstalledactive;
warm/pressure/paging experimentsOFF/fullplanOPEN/performanceAccepted-defaultReadyfalse.
