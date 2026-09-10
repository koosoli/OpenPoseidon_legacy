# Three modernisation tracks: code audit and bounded experiments

September 10 afternoon continuation: remote consolidated-v2 was rechecked at
0038ac20, then advanced with the changes below. Initial baseline and historical
measurements remain explicitly dated. Do not interpret this document as completion
of all three tracks.

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

## Current CPU workload census (same installed 14:00 build)

Measure-ModernisationCpu.ps1 reuses farfield-bench and Measure-AIScale; no new
scheduler or game-side measurement system. Two sequential repeats each, native
1280x720, MSAA4, private preset-3 profile, DLSS/VSync/frame cap off, fixed LOD
governor range 1, real-time simulation, capture at 60 s. SimVehicleCost enabled.
Idle forwarding now also records those inherited diagnostic/LOD flags in metadata.
These are baseline runs, NOT serial/parallel A/B comparisons.

| Scene / repeat | CPU ring mean / p95 ms | sim:step ms/frame | AI ms/tick | vehicles ms/tick | visibility ms/tick |
|---|---:|---:|---:|---:|---:|
| Sparse Field 1 | 15.01 / 16.25 | 0.252 | 0.020 | 0.174 | 0.004 |
| Sparse Field 2 | 14.66 / 15.59 | 0.226 | 0.019 | 0.169 | 0.004 |
| Idle 80 1 | 9.75 / 11.36 | 0.324 | 0.125 | 0.396 | 0.026 |
| Idle 80 2 | 9.92 / 11.07 | 0.319 | 0.120 | 0.387 | 0.027 |
| Idle 320 1 | 18.19 / 18.83 | 1.775 | 0.582 | 0.884 | 0.136 |
| Idle 320 2 | 18.85 / 21.73 | 1.806 | 0.575 | 0.874 | 0.134 |
| Combat 1 | 19.06 / 20.70 | 6.566 | 1.882 | 3.573 | 0.503 |
| Combat 2 | 22.48 / 24.47 | 7.605 | 2.162 | 3.729 | 0.418 |

Ring = final 256 frames. Tick columns = average of the last ten per-second
simulation reports; they have DIFFERENT denominators/windows and must not be
added to per-frame phases. Dedicated physics scope rounded to 0.000 in these
fixtures, not proof collision/locomotion is free (inside vehicle/man stages).
Combat run 2 last report: man 3.085 ms/tick, 193 calls/tick, including nested
man.pilot 1.726 and man.base 1.341 ms. Animation is not separately attributed yet.
Script time rounds to zero; Field is a one-player sparse control, NOT a normal
campaign-script baseline. Combat contains both factions/move orders; the inspected
capture shows battle smoke and logs show active strategic planning and 193 man
updates, but battle state diverges between real-time repeats. Do not use this
variation as a claimed optimisation or deterministic replay acceptance.

For Idle320, land:obj is 2.48/2.51 ms/frame while producer-window waits are
9.14/8.90 ms. Worker queue-submit snapshots are 13.51/13.76 ms and encode costs
are much smaller. Submit may contain driver/GPU waits; moving it to more CPU
workers is not a demonstrated remedy. GPU totals are single asynchronous samples,
not the CPU ring's mean; combat GPU 28.74/19.50 ms must not be called its average
FPS. Screenshot-contaminated ring maxima are excluded from workload-spike claims.

Process private / working-set peaks in GiB, run 1 then 2:
Field 2.295/0.919, 2.286/0.907; Idle80 2.219/0.841, 2.194/0.817;
Idle320 1.968/0.588, 1.960/0.586; Combat 2.193/0.832, 2.170/0.811.
Tracked object GPU residency MiB: Field 52.799 both, Idle80 33.938 both,
Idle320 34.022 both, Combat 58.762/62.846. These counters omit other allocations;
lower Idle320 process memory does not mean adding soldiers saves memory. Different
world visibility/caches and OS working-set decisions prohibit that interpretation.
One-second process samples and raw JSON/logs accompany every capture.

Evidence paths (each run-01 and run-02):
- build/modernisation-cpu/cpu-20260910-143144-Field
- build/ai-scale/cpu-20260910-143356-Idle80-idle-80-Near
- build/ai-scale/cpu-20260910-143606-Idle320-idle-320-Near
- build/modernisation-cpu/cpu-20260910-143818-Combat

Decision: keep pipeline-first priority. No blanket AI threading. Next isolate
combat man.pilot/man.base and strategic-query dependencies, and distinguish GPU
submit waits from encoding before selecting a pure batch. Complete campaign,
Fusion traversal, exclusive animation timings, full-run tails, driver VRAM and
cross-core tests before calling the requested census complete. Existing native
traversal pairs cover the streaming workload, not those missing cases.

## Follow-up: main-thread bank-table admission gap

POSEIDON_SLOW_FRAME_TRACE=1 now logs individual >100 ms frame phase records and
splits draw initialization at residency/snapshot/renderer/objects/lights boundaries.
Off by default, no scheduler changes; timings include waits. Initialization's
residency bucket includes the small suppression/clear setup preceding the call.
This is attribution, not an optimisation. Native route 150738-slow-trace on
8c5f7f9c and 151412-slow-trace on 2ef66ea8 completed all 40 waypoints. The latter
harness reports exit 0 but also cleaned up a leftover owned process; do not use
it as proof of clean shutdown. No renderer validation errors were detected.

In the latter route frame 1169 took 164.170 ms, including 153.093 ms residency;
snapshot/renderer/object/light setup together were 0.021 ms. Yet logged cold+warm
installation was only 28.4 ms. Source inspection identifies an uncharged stage:
LandSave.cpp builds AdapterBankTables BEFORE incrementing coldAttempted and
starting the ObjectCreate timer, then continues to the next ready IR. Many
main-thread header/material resolutions can therefore run despite the two-cold-
installs limit. Moving all AI to workers cannot fix this.

The bounded experiment adds a SEPARATE two-table-preparations/update allowance
inside the existing native residency loop. Deferred IRs stay in the preparer's
existing Ready ownership; warm objects and completed conversions can still be
installed. Non-native worlds and explicit preloading retain prior behaviour.
WGR_OBJECT_STREAM_TABLE_BATCH=0 is the unbounded same-binary control. Bank-table
count/time are now logged separately from installation; byte budgets and splitting
one oversized material remain unresolved. A count cap is not a hard ms guarantee.
Acceptance requires same-route tail improvement AND comparable residency progress,
not just a nicer ending ocean screenshot. Paired acceptance is recorded below.

Three alternating same-binary pairs on cd74a58a (installed 15:23), with the same
native route/profile as above, clutter reuse ON in both arms, slow trace ON in
both arms. Run names share build/native-stream-cancellation/cancel-20260910- and
the suffix -slow-trace; unbounded arms also append -legacy-tables.

| Unbounded / two-table run | Moving trace p99 ms | Logged moving maximum ms | Static trace p99 ms | Logged moving frames >100 ms |
|---|---:|---:|---:|---:|
| 152310 / 152528 | 97 / 81 | 265.408 / 162.550 | 38 / 91.5 | 11 / 7 |
| 152745 / 153003 | 86 / 93 | 294.240 / 153.451 | 40 / 131 | 11 / 9 |
| 153221 / 153438 | 87 / 90 | 296.622 / 167.178 | 39 / 101.5 | 11 / 10 |

Trace percentiles average gaps (moving coverage about 96%); slow-frame count/max
above are the separate exact main-loop records, not averaged gaps. These do not
cover work outside FrameProfiler. Moving median stays 29 ms in all six runs;
throughput FPS is 34.6/34.0, 33.6/33.9, 33.6/33.9: no convincing FPS gain.
Max table count/update 24 -> 2 in every pair, no bound violations. Max bank-table
time/update ms: 853.457/362.213, 714.206/460.034, 905.720/388.810. A single
material can still be expensive; counting is not equivalent to doing less work.

At the static-phase marker, last logged native residency is 19999 in controls
versus 19689/19596/19655 with the cap. Both arms reach 19999 by movement start
and 19977 at movement end; these are rate-limited log observations, not exact
per-frame visibility counts. All six final captures have 22370 registered
instances and 1923.552 MiB tracked GPU residency, but show ocean, not a visual
proof that the initial missing 310-403 placements were harmless.

Private-memory peak GiB pairs: 11.060/11.054, 11.017/11.063, 11.055/11.109.
Working-set peaks: 6.861/7.152, 7.101/7.120, 7.123/7.180. No memory win claimed.
All routes/captures completed; the old harness sometimes emitted a leftover-PID
cleanup warning despite recorded exit 0. Cleanup now retains the original Process
handle and checks HasExited, instead of rediscovering a potentially recycled PID.
The standalone contention check also now includes the actual OpenPoseidon name;
the outer game lock already checked it. Older warnings are not shutdown proof.

**Decision: do NOT enable the table cap by default.** It reliably bounds a real
uncharged stage but shifts stalls into the static phase, slightly delays initial
coverage, and has no consistent p99/FPS improvement. Default returns to 0;
WGR_OBJECT_STREAM_TABLE_BATCH=2 remains an explicit experiment. Keep diagnostics,
cancellation hardening and the separately accepted clutter reuse. The next loading
experiment must reduce/prepare individual native bank/material work within the
existing ownership pipeline, with a serial reference, instead of relabelling
delayed work as a speedup. This is not a completed end-to-end byte-budget pipeline.

The frame analyzer's gap warning was corrected too: averaged gaps can distort
p99 in EITHER direction, even while concealing the worst individual frame. A fifth
regression test demonstrates this; no blanket lower-bound claim on p99 remains.

## Accepted follow-up: bounded BC7 preparation on the existing TaskPool

Instead of a new scheduler or wholesale AI threading, isolate the actual native
texture cost. a39dd881 adds opt-in POSEIDON_DDS_PREP_TRACE=1 in DdsImport.cpp,
TextureSourceDDS::Init. Read includes mount access/decompression and name setup;
decode includes EDDS parsing, block decoding and channel preparation. Coverage
read/decode, merge, tint and finalisation are separate. GPU upload is NOT included.
Only calls >=2 ms are logged, so summing these records excludes short calls.
Failed calls keep success=false and unfinished stage time in finish. Byte counts
are input files and retained output mips, not a complete RAM/VRAM peak metric.

Native route cancel-20260910-161933-slow-trace-legacy-tables-dds-prep on a39dd881:
1353 logged calls total 22126.661 ms, read 4752.597, decode 15344.360, merge 393.131,
tint 1192.078. These are cumulative overlapping-with-other-thread elapsed scopes,
not a 22-second frame. A shared tinted metal tile alone repeatedly spends about
25 ms decoding before its 2 ms tint. This justifies targeting decode first, not
assuming tint or AI is the dominant remaining cost.

f79ac2a9 adds DecodeBc7ImageBatched in Graphics/Textures/BlockCompression.cpp:
64-pixel-row batches on Core/TaskPool, immutable input and disjoint output,
joined before existing tint/material/GPU code resumes. DdsImport uses it only
for BC7 mips >=256x256 when Foundation::IsMainThread and a global pool exist.
External preparer threads remain serial (they are NOT registered enki threads).
No new lifetime, bank mutation, RNG, simulation ordering, cache or GPU ownership.
The serial decoder remains the reference; 1-thread pools fall back to it.

Unit tests compare all eight modes, invalid blocks, odd edges, guard bytes and
1/2/4 pool threads. BC/EDDS/PAA suite: 46 cases / 9829 assertions pass. In-game
mode 2 additionally checks every eligible real mip against serial output:
cancel-20260910-163111-slow-trace-legacy-tables-dds-prep-bc7-2 gives 1882 identical
checks, no mismatches, exit 0. Its doubled decode work is NOT a performance arm.

Three alternating pairs on the SAME installed f79ac2a9. Same 4 km route,
1280x720 native, DLSS off, MSAA4, preset3, fixed AutoLOD1, clutter reuse on,
bank-table cap OFF in both, slow/DDS traces on in both. Host: RTX3070 Vulkan,
8 existing task threads / 16 logical cores. Fresh process per arm; OS caches
not flushed, so these are repeat-process tests, not cold-disk claims.
Run IDs under build/native-stream-cancellation/cancel-20260910-, suffix
-slow-trace-legacy-tables-dds-prep; parallel arms additionally append -bc7-1.

| Serial / batch run | Moving FPS | Moving p95 ms | Moving p99 ms | Exact logged moving maximum ms | Logged moving >100 ms | Static p99 ms |
|---|---|---|---|---|---|---|
| 163419 / 163636 | 33.8 / 33.7 | 42 / 44 | 90 / 87 | 277.286 / 162.390 | 10 / 6 | 38 / 39 |
| 163852 / 164111 | 33.9 / 33.6 | 42.5 / 43 | 91 / 84 | 254.484 / 165.841 | 9 / 6 | 40 / 38 |
| 164327 / 164546 | 33.3 / 34.0 | 42 / 41 | 87 / 88 | 275.880 / 183.793 | 10 / 7 | 38 / 40 |

FPS is trace throughput, not reciprocal median. Percentiles have ~96% one-frame
counter coverage; gap averaging can distort p99 either way. Exact maxima/counts
use separate main-loop records between FFCAM move/done. Static/moving transitions
and final auto-screenshot are not treated as traversal spikes.
Max bank-table burst ms pairs: 860.729/404.445, 832.960/391.523, 924.699/420.486.
For the SAME tinted st_metalpaint_mate_1m_bcr material, decode ms pairs are
23.184/7.699, 27.904/7.906, 24.579/6.425. This illustrates attribution, not an
aggregate speedup for every material. The >=2 ms log threshold forbids comparing
raw cumulative sums as though both arms logged every identical call.

All six runs exit 0, complete the route, retain 22370 final registered instances
and 1923.552 MiB tracked GPU residency. Last logged residency at move start is
19999 in every arm; move end is 19977 or 19996 as a rate-limited recentre log,
not exact per-frame visibility. Private-memory peak GiB pairs:
11.017/11.065, 11.031/11.070, 11.049/11.022. Working-set peak pairs:
7.109/7.273, 7.094/7.284, 7.140/7.105. No memory-saving claim. Driver-total VRAM
was not sampled in these pairs; tracked residency is not a substitute for it.

Decision: enable this bounded path by default for shorter loading spikes. Keep
POSEIDON_DDS_BC7_BATCH=0 for serial A/B and 2 for the byte oracle. Diagnostics
remain OFF by default. No consistent average-FPS or p99 gain is claimed; the
repeatable benefit is lower worst loading bursts, with identical decoded bytes.
The original table-count cap remains OFF: delaying work and reducing its cost
are different changes with different acceptance results.

Still open: full campaign/Fusion workload census, driver VRAM timelines, native
multiple-view/teleport/world-change stress, and cross-core runtime performance.
This synchronous join is NOT complete async preparation or a global byte budget.
Next reuse already decoded shared native tiles within existing ownership/budgets,
or prepare one owned DDS payload ahead of bank registration; never call the
file server/texture bank from arbitrary worker threads. Far cluster HLOD remains
a separate, visually gated experiment, not something the Satmap already does.

Final default acceptance: ae8c90fa installed 16:53, native route
cancel-20260910-165344-legacy-tables-dds-prep-bc7-default with no BC7 override,
exit 0, complete camera route, no validation failures/timeout/contention,
22370 registered instances. Ocean endpoint image inspected; not village quality
proof. Combined BC/EDDS/DDS/PAA/preparer unit suite: 81 cases / 10314 assertions.

## Prepared payload ownership hardening

Code audit found two smaller correctness/locking gaps in the EXISTING shared
PreparedTextureStore (not a new cache/scheduler): Take accepted a decoded-image
entry as an empty block chain and destroyed its RGBA payload; SweepLocked and
MarkUploaded moved block bytes outside the mutex but freed image bytes inside.
Take now requires the requested payload kind without consuming the other kind.
Expiry/upload retirement move both payload types out before their storage is freed.
Budget, TTL, upload marks, world Clear and GPU-thread ownership remain unchanged.

Six targeted PAA/streaming cases (68 assertions) pass, including new symmetric
wrong-kind probes and decoded upload/budget/eviction checks. The client runtime
smoke is still pending. No observed frame stall is attributed to this gap yet;
this is ownership hardening, not a measured FPS gain or completed native-DDS cache.

Installed 0a19a9cb native smoke: cancel-20260910-221113-slow-trace-legacy-tables-
dds-prep-bc7-default. Exit 0, 4 km route completes, 22370 registered instances,
1280x720 native/MSAA4/AutoLOD1, table cap off. Final ocean image inspected, not a
village-quality acceptance. This verifies lifecycle after the payload change;
one run does not establish a performance improvement.

Test-NativeStreamCancellation.ps1 adds optional -SampleDriverMemory. One owned
nvidia-smi process samples at 1 Hz across load/traversal/shutdown and is terminated
in finally. CSV reports local timestamp, GPU index/UUID, total/used MiB and GPU
utilisation; metadata explicitly labels whole-device occupancy including other
applications. This is not renderer tracked residency or process-private VRAM.
Use the same sampler on both arms; its overhead is not subtracted. Missing tool
or missing samples fails a specifically requested telemetry run instead of
silently reporting zero. PowerShell syntax check passes; end-to-end run pending.

The first driver-memory run (cancel-20260910-221905) is INVALID: its verbose
93-character staged mission identifier exceeded GlobalHeader's filename[80].
SetMission's strcpy overwrote the adjacent world name, so StartAutoTest could
not boot the scene. The 117 driver samples only cover an empty world and must
not enter performance comparisons. The owned sampler did terminate correctly.
Mission copies are now bounded, unsupported identifiers rejected without
changing the save/network header layout, and the harness stages short unique
identifiers independently of descriptive capture labels. Mission-loader tests:
17 cases / 84 assertions pass, including 79/80-byte boundary and adjacent-field
integrity checks. Installed runtime repeat remains pending.

Installed d8db97c5 repeat: cancel-20260910-225129-slow-trace-legacy-tables-
dds-prep-bc7-default-driver-memory passes the full route, 22370 registered
instances, exit 0, 138.8 s wall time. 138 whole-device driver samples span
1538..5259 MiB; the ocean endpoint image was inspected. No FPS improvement is
inferred from the cheapest endpoint frame or compared against the invalid run.
The harness now defaults to table cap 0, matching production; the rejected cap
requires explicit -ExperimentalTableBudget N. -LegacyTableBudget remains an
accepted compatibility alias for the production path.

## Narrow native tint-decode reuse experiment

The valid 22:11 trace contains repeated colour-tile decodes: woodplanks01 42
calls / 307.19 ms cumulative decode, metalpaintcoated 32 / 289.31 ms and
metalpaintmate 32 / 269.06 ms (only logged calls >=2 ms; NOT frame-time savings).
An opt-in POSEIDON_DDS_TINT_REUSE=1 experiment reuses pre-tint decodes only for
non-composite tinted colour sources. Existing PreparedTextureStore owns them,
sharing TTL, world Clear and its byte budget, with a subset cap of the smaller
of 64 MiB or one quarter of the configured store budget. No other preparation
entry is evicted to admit these snapshots. Pixel/source copying and retirement
happen outside the mutex. Exact source-byte comparison and the decode-resolution
key prevent reuse across changed input or cap; the actual VFS read still runs.
Active local input/output copies and map overhead are not a total-RAM guarantee.

Source pixels are copied before each material's tint; no coverage-composition,
bank registration, GPU ownership or simulation path changes. Optional
POSEIDON_DDS_TINT_REUSE_VERIFY=1 redecodes every hit and compares all mip pixels
and metadata before allowing it, falling back and logging MISMATCH on failure.
Verification runs are not timing comparisons. Harness flags -TintReuse 0/1 and
-VerifyTintReuse select the arms. 27 EDDS/PAA/streaming cases / 480 assertions
pass including the final resolution-key hardening; installed A/B remains
pending. No default activation or performance gain is claimed yet.

Installed d9e6623b verification 230952: 144 hits redecoded byte-identically,
complete route/shutdown, 22370 registered instances. No verification timings
are used below. Measured runs share 1280x720 native/MSAA4, governor range 1
(min/max LOD width equal), table cap 0 and the driver sampler:

| Run suffix | Reuse | Moving median/p95/p99/worst ms | Throughput FPS | Private peak MiB | Device peak MiB |
| --- | --- | --- | --- | --- | --- |
| 231252 | 0 | 30/46/94/211 | 32.3 | 11269.7 | 4915 |
| 231509 | 1 | 29/44/100/271 | 33.1 | 11390.3 | 4915 |
| 231915 | 1 | 29/48/96/150 | 33.4 | 11425.2 | 4916 |
| 232626 | 0 | 30/47/103/350 | 32.7 | 11348.2 | 4879 |
| 232843 | 1 | 30/49/98/280 | 32.4 | 11328.8 | 4878 |

These are `cancel-20260910-<suffix>-slow-trace-dds-prep-bc7-default-driver-memory-
tint-reuse-<arm>`. FT deltas have 95.4..97.0% one-render-frame coverage, so tails
are approximate, not exact full-frame percentiles. All five finish with 22370
registered instances. Logged tinted-source decode totals fall from 4420..4443 ms
(548 calls) to 3531..3571 ms (546 calls, 141 logged hits); only calls >=2 ms are
logged, and this is cumulative CPU-stage time, NOT frame-time savings.
No stable tail-latency, FPS or memory advantage is established. Keep reuse OFF;
avoid more cache complexity before the still-synchronous bank/material stage
is addressed. The 271 ms candidate frame spent 265.7 ms in residency, including
188.2 ms preparing seven bank tables and multiple previously unseen sources.

Run 232131 (reuse OFF) is excluded: a shutdown watchdog fired after 290 s,
following a completed screenshot but no `Shutdown complete`; metadata says
status=failed/timed_out=true. This is not evidence that reuse caused the hang.
The native wrapper previously ignored the child's exit code and accepted the
remaining metrics. It now propagates the exit failure and independently requires
successful, uncontended, non-timeout metadata and a completed shutdown marker.
Assert-CaptureLifecycle accepts the good 231252 artifact and rejects 232131.
Root cause of that shutdown hang remains open; do not silently discard failures.

## Native archive failure ownership

Inspection of the existing native model worker route found a concrete race:
EnfusionMount::Read called PakArchive::Read, whose failure path const_cast-wrote
the shared `_error` string. Independent file streams did not make that error
mutation safe. PakReadRequest now owns a copy of archive path/entry metadata and
writes only caller-owned output/error. Mounted reads use this path; the legacy
single-caller archive diagnostic API is retained. This is not a new scheduler,
async DDS pipeline or a proven fix for the shutdown hang above. Mount Open/Close
still require existing worker quiescence; a request does not pin an on-disk file
against external editing. PAK/EDDS/PAA suite: 28 cases / 502 assertions pass,
including eight workers doing 16 success/failure read pairs with independent
errors, compressed/plain payloads, and short/missing-input rejection.
Installed smoke after this archive change remains pending.

Installed 4dd6a906 smoke 233954-slow-trace-bc7-default-driver-memory-tint-reuse-0
passes route, strict lifecycle checks and shutdown, with 22370 registered
instances / 134 driver samples. This does not establish that the earlier
intermittent hang is fixed.

Follow-up audit found active conversion jobs releasing their final bank-table
references under ObjectStreamPreparer's queue mutex (and failed-job models in
the same scope). Retirement now explicitly drops tables/models/shapes outside
that mutex, including stale results. A destructor callback that re-enters
SnapshotStats verifies the lock boundary, rather than merely testing conversion
success. Conversion also has its own `objconvert` watchdog scope; previously
only parse/texture preparation was watched. Nine preparer/ShapeAdapter cases
pass: 189 assertions with normal settings; 992 assertions with concurrent
adaptation enabled and eight workers. Runtime verification of this follow-up
is still pending; no proof yet that it caused the observed shutdown hang.
