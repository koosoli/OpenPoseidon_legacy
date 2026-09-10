# Three modernisation tracks: code audit and bounded experiments

Baseline: safety/consolidated-v2 = 2ad0a9becb3b46e45ec42774d025f8e9fff23f41.
Worktree: asset-obs. The unfinished local TerrainEditJournal is NOT part of
that baseline. Analysis requested first; implementation subsequently authorised.

## Evidence limits

This audit reads the implementation, September handover, PARKED, the post-DLSS
roadmap, PERF-021/024/029 and the native Reforger decision and raw captures.
Historical measurements below are not new four-scene measurements of HEAD.
The latest forest capture uses an older build stamp with the wind change later
committed in the baseline. No current controlled comparison of all four requested
workloads, including RAM/VRAM timelines, has yet been completed. Consequently the
priority is provisional, not a measured prediction of FPS gains.

Subsequent implementation evidence is below. It supplies a controlled native
streaming-route comparison, NOT the still-missing complete four-workload census.

Do not reuse old roadmap headlines as current bottlenecks: render overlap already
exists, PERF-021 corrected an unconditional screenshot wait, and later changes
altered both retained coverage and soldier palette uploads. Likewise, the native
loader is no longer the old fully eager placement path described in older sections.

## 1. Distant worlds

**Present:** TerrainWgpu.cpp BuildCdlodTree/SelectVisibleCdlod select terrain
geometry; gfx3d/cull.rs retains instances and selects per-model LODs on the GPU.
LandLoadEnfusion.cpp maps native authored LOD thresholds, including low-triangle
vegetation impostors. These are individual model LODs, not a town/forest HLOD.
LandSave.cpp UpdateModernObjectResidency supplies distance-ordered object residency.

**Partly integrated:** TerrainWgpu::BuildAutomaticSatmap builds OFP far albedo.
terrain/terrain.wgsl, sample_authored_cell, blends it over 800-2400 m and returns
before the legacy sample at the far end. This really avoids that fragment sample;
it does NOT remove object geometry or its shadows. UploadGroundTextures builds the
automatic map in the non-paged legacy path, after binding the legacy textures.
Therefore no detail-residency saving is demonstrated for this path. The generated
texture samples terrain colours, not forest crowns/building silhouettes.
Authored A2/A3 paging and shader detail early-outs also already exist; do not
describe them as a new universal native-Reforger solution.

**Missing in the inspected path:** cluster-level HLOD substitution and a proven
OFP far-only residency mode. A fixed object-count/window budget cannot promise
complete distant settlements on dense worlds. Satmap alone cannot fix this.

**Smallest experiment:** one distant forest block, using the existing instance
and LOD infrastructure with a coarse cluster representation. Mutually exclusive
near/far submission outside a bounded transition band, including shadow views;
count both paths to catch double work. Compare against a separate terrain-only
far-residency experiment before choosing which gives useful savings. Keep near
materials, view range and fog unchanged. Do not bake time-of-day shadows into albedo.

**Expected benefit (unmeasured):** more complete far silhouettes, potentially fewer
draws/triangles/shadow triangles. Terrain paging primarily targets memory/load work.
Additional LODs are a quality/performance tradeoff, not lossless optimisation.

**Risks/tests:** oblique and aerial views, sun angles, wind, destruction invalidation,
multiple cameras/reflections/shadows, transition shimmer under native and DLSS,
no hidden holes and no double shadows. Compare image crops plus geometry/material
and residency counters, not FPS alone.

## 2. Targeted CPU work

**Present:** Core/TaskPool wraps enkiTS (ParallelFor and owned Submit tasks).
Landscape.cpp parallel terrain queries and TerrainWgpu preparation already use it.
ObjectStreamPreparer has bounded queues, dedicated below-normal-priority workers,
main-thread FP environment, native XOB/loose-file parsing and ShapeAdapter conversion.
EngineWgpu::RenderThreadMain, producer windows and snapshot handoff already overlap
simulation and rendering. No scheduler replacement is justified by this audit.

**Evidence:** PERF-021 records historical combat CPU mean 23.8 -> 21.9 ms after
removing the unconditional screenshot wait. PERF-024 attributes 6.42-6.83 ms to
draw encoding in its then-current combat build, and explicitly separates acquire
wait from setup work. These are already-landed results, not gains available again.
The 80/160/320 actor Measure-AIScale fixture is idle, not active combat.
PERF-029's memory-pressure explanation is a hypothesis, not measured causality.

**Remaining gap:** a current cross-workload critical-path census. A static native
forest capture with sim:step 0.09 ms cannot justify threading combat AI. Conversely,
an idle camera-away control cannot prove combat has negligible AI cost.

**Smallest experiment:** refresh the four-scene baseline first, then select one
pure preparation/query batch only if its cost lies on the critical path. Prefer
eliminating repeated work. Reuse TaskPool with immutable input, indexed output and
stable main-thread commit order. Do not parallelise AIUnit::Think or bank mutation.

**Expected benefit:** CPU-bound FPS or shorter producer stalls, conditional on
measurement. No assumed gain for a GPU-bound forest or water view.

**Risks/tests:** serial/parallel oracle, RNG order, mission commands, save/reload,
MP authority, entity removal while a job runs, FP state, oversubscription, task
granularity and render-thread contention. Test core counts as well as actor counts.

## 3. Complete the existing loading pipeline

**Present:** ObjectStreamPreparer Request/Query/Take/SubmitConvert/TakeConverted,
request coalescing, world generations, distance-order queues and stale-ready eviction;
PreparedTextureStore, residency pressure clamp, texture leases and GPU retirement;
LandSave admission modes/count/time/upload-byte levers. TextureWgpu::EnsureUploaded
and EngineWgpu producer windows preserve GPU resource ownership. Extend these.

**Partly integrated:** worker preparation does not cover all native material work.
EngineWgpu::ResolveMaterialLayers still resolves layers through bank Load and
EnsureUploaded at registration. Texture preparation for the inspected native runs
has zero prepared hits; this is not evidence that native materials need no work.
ReadyLimit counts parsed Ready IRs, not all stage-3 memory (already identified in
the preparer tests). Count bounds are not byte budgets. Do not add a second manager.

**Measured raw evidence:** Forest-wind-bounded-1303/run-01.json records 487 async
models and 7 synchronous cold fallbacks. Cumulative cold optimize = 7906.8 ms,
texture headers = 2521.2 ms, texture fallback = 1776.6 ms, texture create = 1565.1 ms.
These scopes must not be summed as disjoint frame costs. Worker total = 23029.3 ms;
161 ready-full parks. This indicates preparation/installation imbalance, not a
23-second main-thread stall. Tracked GPU residency = 1834.6 MiB, not total VRAM.
The settled 256-frame sample mean/p95 is 24.08/25.42 ms at DLSS Quality
1286x724 -> 1920x1080, MSAA4. Its frame_max is NOT an accepted traversal spike:
the capture operation can contaminate it. AutoLOD was not fixed for prior pairs.

The prior native-prefetch correction removed unused converted-PAA preparation:
roughly 7 seconds cumulative worker preparation and hundreds of MiB of prepared
RAM disappeared. It did not demonstrate higher FPS or lower tracked VRAM.
The upload-byte cap was already measured null for a single expensive object;
the remaining overshoot is budget plus one indivisible admission.

**First bounded implementation:** correct same-world camera-window cancellation.
DropStale previously ignored active Parsing/Converting and queued conversions;
their completion could repopulate unneeded data until another recentre. Mark active
slots stale, reject stale completion, remove queued conversions, release their
resources outside the lock, and allow a returning desired set to reuse active work.
Do not free/reassign an in-flight slot before its worker exits. World generation
checks remain first, before indexing the current world's slot vectors.

**Next experiment:** instrument and prepare ONE native material family off-thread,
with owned CPU data and byte accounting across IR, conversion and texture staging.
Reserve/release bytes across stages; allow an explicit oversized-item progress rule
to prevent deadlock. Split registration/upload into bounded chunks only after its
single-object cost is measured. GPU creation stays in the existing ownership path.

**Expected benefit:** shorter loading spikes and bounded memory first; steady-state
FPS is a separate claim. Cancellation is lifecycle hardening, not an FPS win.

**Risks/tests:** teleport away/back, queued/active/ready cancellation, world reset,
multiple-view desired-set union, failed/retried uploads, pressure hysteresis,
oversized assets, geometry/material availability together, deferred retirement
while snapshots still reference resources, repeated tours with a memory plateau.
Do not let asynchronous completion change authoritative simulation availability.

## Order and acceptance protocol

1. Pipeline lifecycle and attribution: smallest proven gaps, existing framework.
2. One far-representation experiment: important visual completeness goal, larger
   quality risk; native shadow cost makes this worth investigating, not guaranteed.
3. New simulation parallelism only after updated profiles establish a candidate.
   Pure preparation work from item 1 may run on existing workers earlier.

Use normal mission, visible idle 80/320 soldiers, seeded active combat, and the
same scripted fast-flight route on native Everon and Fusion. At least three
alternating arms; separate cold-cache from repeat-tour runs. Pin and record build,
assets, profile, resolution, DLSS mode, MSAA, view range, shadows, weather/time,
FPS cap/VSync and fixed LOD governor. Add a separate production-governor validation.
Record per-frame CPU/GPU and named waits, p50/p95/p99, counts over 33/50/100 ms,
request-to-use latency, upload bytes/time, process private/working-set RAM, tracked
and driver-reported VRAM where available. Mark unsupported counters unavailable.
Do not count screenshots/startup in movement-frame percentiles; report them apart.
Report geometry/coverage differences with timings. No missing scenery, extra fog,
physics-engine replacement, wholesale AI redesign or build-system side project.

## Implemented and measured: recurring native clutter preparation

The first fresh route found BakeEnfusionClutter decoding identical definitions and
plant images at each RFG-101 geography refresh (roughly every two seconds).
This was 319-359 ms per call while grass/road coverage stayed unchanged. Instead
of adding threads, geography-only refreshes now reuse the surface runs and existing
atlas. They STILL rebuild per-cell geography and apply current selection/road data.
World, material and UI uploads retain the rebuild path. No second texture cache,
new scheduler, reduced density or extra GPU allocation. The additional persistent
data is one small surface-run vector, not another atlas or whole-world mask.

Also classify modern road model paths once per model during the upload, rather
than once per placement. If no model is a legacy road, skip that placement walk.
The original predicate and out-of-range guard are preserved. This change is common
to BOTH arms below; its separate frame-time gain is not claimed.

WGR_NATIVE_CLUTTER_REUSE=0 is the same-binary full-rebuild control.
WGR_NATIVE_CLUTTER_VERIFY=1 runs the full reference on the SAME input geography
after reuse and checks every output geography word and used authored atlas byte.
Reserved layer 0 and unused layers belong to UploadGrassTuft and are excluded.
Installed run cancel-20260910-140053-verify-clutter: 39 identical comparisons,
zero mismatches, normal exit. Verification is off in production and in timing arms.
The periodic reused bake is approximately 7-10 ms in these runs.

Three alternating same-binary pairs, 2ad0a9be+dirty installed 14:00, native Everon,
1280x720 native (DLSS off), MSAA4, VSync/cap off, WGR_LOD_GOVERNOR_RANGE=1,
same private-profile settings and scripted 4 km / 100 m/s route. Full route markers
are present. Startup and screenshot are not included in the moving-phase trace.
Run names below share build/native-stream-cancellation/cancel-20260910-.

| Pair: rebuild / reuse | Moving p99 ms | Frames >100 ms | Moving throughput FPS | Final-ring p99 ms |
|---|---:|---:|---:|---:|
| 140506-legacy-clutter / 140724 | 356 / 86 | 33 / 8 | 30.4 / 34.1 | 344.90 / 42.24 |
| 140941-legacy-clutter / 141159 | 370 / 101 | 28 / 14 | 29.8 / 33.4 | 359.94 / 45.73 |
| 141419-legacy-clutter / 141640 | 381 / 187 | 32 / 24 | 29.2 / 30.6 | 362.77 / 52.39 |

Do not hide the third pair: reuse still has a 988 ms moving trace outlier and a
1552 ms static outlier; the full-rebuild moving maxima were 448/479/479 ms.
Its final moving FrameProfiler ring also contains a 758.85 ms outlier. Removing
recurring clutter decode does NOT close all loader/driver/producer stalls.
Median moving frames are 27 ms for controls and 29/29/27 ms for reuse; this is a
tail-latency improvement, not a universal reduction of every frame's cost.

Trace counter coverage is 93-97%; gaps are averaged and can understate tails.
The separate FrameProfiler ring has actual float frame times, but only 256 frames.
The old analyzer invented frames from duplicate counters and missed new residency
log fields; corrected before this table, with four Python regression tests.

Every arm ends with 22370 registered instances and 1923.552 MiB tracked GPU
residency. This is not total driver VRAM. Private-memory peaks (GiB) are
11.130/11.061, 11.081/11.070, 11.060/11.088; working-set peaks are
7.223/7.138, 7.149/7.109, 7.149/7.030. One-second CSV samples are beside each log.
No meaningful RAM/VRAM reduction is claimed. Driver VRAM timelines remain absent.
The ending screenshots show ocean: their settled GPU totals are not village FPS.

Acceptance: keep the reuse optimisation and lifecycle correction. Next measure
the remaining individual stalls; do not compensate by enabling the previously
ineffective upload-byte cap or by adding AI threads. HLOD, far-only OFP texture
residency and byte budgets across every preparation stage remain open.
