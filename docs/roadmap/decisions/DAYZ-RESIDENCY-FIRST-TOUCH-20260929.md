# DayZ residency first-touch latency, 29 September 2026

The existing 11-pose ChernarusPlus route was run with the installed `bf76fa15`
EXE/DLL pair, at 1280x720 with the script's fixed 60 fps cap and optional
streaming experiments disabled. The initial screening
`build/stream-residency/stream-sep29-disabled-20260929-145904` passed, exited
normally and returned to 19,923 desired/19,923 resident objects with no
required identities outstanding or pending residency. Its `drw:init` mean was
23.644 ms, p95 131.596 ms and maximum 848.471 ms. These are frame-phase
measurements, not a matched before/after performance result.

Script commit `41841d0f` adds `-FramePaceTrace` to the same route. It propagates
the opt-in `POSEIDON_FRAME_TRACE` path through the game-lock relaunch, requires
nonempty main/producer/worker CSV files after normal exit and records the flag
in provenance. The bounded trace run
`build/stream-residency/stream-pace-sep29-disabled-20260929-150424` passed
with 693 frames and no dropped samples. Large `draw_init_ms` spikes were not
explained by corresponding `predraw_wait_ms` worker waits.

One further run enabled the existing `POSEIDON_SLOW_FRAME_TRACE` split via
`-TerrainPageTimings` and also captured frame pace:
`build/stream-residency/stream-init-sep29-disabled-20260929-150715` passed,
exited normally, and captured 781 frames without dropped samples. The trace
assigns frame 20's 119.634 ms, frame 52's 431.604 ms and frame 58's
563.189 ms of init work to `Landscape::UpdateModernObjectResidency`; snapshot
and renderer setup in those rows are negligible. `drw:init` mean was 17.31 ms,
p95 117.98 ms and maximum 1029.95 ms. These three route runs have different
instrumentation/cache/async histories and do not establish a causal FPS change.

The cold-admission log has 733 model rows summing about 20.4 seconds of
owner-thread admission in the instrumented route. The largest first touches
are `city_school` 812.3 ms, `village_policestation` 595.5 ms and
`tenement_small` 554.7 ms. On frame 58, `tenement_small` caused the
563.189 ms residency phase and uploaded 81 textures; the logged texture-read
bucket was 394.4 ms, creation 25.7 ms and alpha scan 38.8 ms. Its worker
model parse and adapt were already complete, so reducing the per-update
cold-model count cannot remove this single-model tail. No visual approximation
or default policy change is authorized by these timings.

The next useful source work is an exact, bounded owner/worker handoff for the
material texture dependencies of an actual first-touch model. Existing
`CollectObjectStreamTextureNames` does not enumerate DayZ `.rvmat` stages;
material resolution and upload occur on the owner during registration. The
older warm-stage preflight did not prove a same-source Take/upload and remains
off. A worker must not read the unlocked global file-server state or guess
material-to-texture identity. Preserve source binding, mount precedence,
material role, cancellation and complete building assembly. Require a
same-binary controlled ON/OFF route with normal return and no worse measured
latency before promoting any new path. GeometryPage, Fine release and pressure
control stay off by default; `performanceAccepted=false`, `defaultReady=false`.

## Bounded initialized-primary prototype

`ObjectStreamPreparer::SubmitConvert` now has one narrower opt-in slice when
**both** `WGR_OBJECT_STREAM_PBO_TEXTURES=1` and
`WGR_OBJECT_STREAM_COLD_TEXTURE_HANDOFF=1` are set. On the owner thread it
visits the already-built texture tables first, accepting at most 64 distinct
PAA entries with a live `CaptureColdPaaRead` source binding. It passes only
immutable archive-member requests to the existing worker `PreparePackedTexture`
path and charges the retained source metadata to the conversion reservation.
An absent, uploaded, unbound, oversized or refused texture keeps ordinary
admission. The older name-based capture remains for other IR face textures;
the source-bound entry wins for duplicate keys. This has no new default-path
work, does not enumerate RVMAT stages or make a prepared texture necessary for
geometry progress.

The paired game/test desktop build passed. A new actual mapped-PBO test with
two initialized model textures passed 34 assertions: one texture stayed in
the existing owned-primary chain and the second reached a source-bound
PreparedTextureStore entry, whose exact member could be claimed. The prior
six cold-handoff cases passed 563 assertions. This proves the opt-in worker
handoff for that original PAA fixture, not a DayZ performance gain or material
completeness.

The first installed combined-option route stopped at the harness's initial
sample, because it asserted that all `warmPrepared*` counters were zero when
warm jobs were disabled. Those counters belong to the shared store, which the
new cold path also uses. Its owned game exited normally, with no forced stop.
Script commit `6c6cc9d7` keeps all warm-job queue/work counters strictly zero
but permits shared-store counters only for the explicit combined option; it
also records that combination in provenance. The repaired installed route
`bound-paa-retry-sep29-disabled-20260929-153127` passed with normal shutdown,
877 frames, no dropped samples and full 19,923/19,923 returned residency.
It recorded 85 source-bound Puts, 45 Takes, 45 successful uploads and
83,880,704 upload bytes, with zero warm jobs and zero source refusals.

For a bounded comparison, the same installed `2233926b` EXE/DLL pair ran
`packed-only-sep29-disabled-20260929-153344` with the existing PBO texture
preparation but without cold handoff. It also passed normally and returned
19,923/19,923, with 899 frames and no drops. Both runs were at the script's
1280x720/4x MSAA/60-fps cap, fixed route, water backend 0, simulation mode
Disabled and no frame-phase instrumentation. The combined route's `drw:init`
mean/p95/max were 12.542/62.390/975.937 ms; packed-only measured
12.530/49.900/1004.390 ms. `tenement_small` admitted in 287.8 ms with
41 prepared texture uploads in the combined run, versus 232.3 ms and the
same 41 in packed-only. `city_school` was 745.0 ms versus 611.7 ms, with
21 versus 18 prepared uploads. The existing PBO path already served the large
primary dependency set, so this new source binding has not shown an additional
DayZ latency benefit. The runs have different asynchronous completion and
cache histories, and enabling cold handoff also activates the existing
owned-primary path; these rows are screening evidence, not an isolated causal
effect of the new capture. Keep both options off by default and do not claim
no-regression or promote them from this pair.

## Exact raP NormalMap before first conversion

Commit `b1792bcc` adds a separate default-off early path for the two already
selected DayZ spruce-tree raP fixtures. On the owner thread, `SubmitConvert`
rechecks the current physical material member, reads at most 256 KiB of that
captured raP, extracts its NormalMap role, and asks the WGPU owner callback for
the current initialized PAA binding. Only the exact
`dz\plants\tree\data\t_piceaabies_trunk_no.paa` member, at most 16 MiB, is
passed to the existing conversion worker. The worker prepares it before
publishing `Converted`; failed capture/preparation leaves ordinary material
loading available. The earlier late warm-stage path remains as fallback. The
feature requires the existing source, warm-job, material-preflight and
warm-stage-capture options plus `WGR_OBJECT_STREAM_RAP_STAGE_EARLY_PREPARE=1`.
All remain off by default. `Test-DayZStreamingLatency.ps1 -RapStageEarlyPrepare`
sets this exact combination and records it in the route provenance.

A synthetic mapped-PBO test passed 19 assertions, including source-bound worker
Put before the owner takes Converted and an exact-member Take. The paired
game/test build passed; the committed `b1792bcc` EXE/DLL pair passed installed
WGPU startup. One installed DayZ route,
`build/stream-residency/rap-early-sep29-disabled-20260929-155623`, passed with
normal exit, 908 frames, no dropped samples and returned 19,923/19,923
residency. For model index 72 the log orders `accepted`, `storePut`,
`workerPut` at 15:56:39.046-39.066, then owner `storeTakeWarm`,
`sourceValidated`, a successful 2,796,032-byte upload and WGPU NormalMap bind
at 15:56:39.195. The source member was 1,780,450 bytes. This is real
same-source first-registration handoff for that tree NormalMap, not proof of
pixel parity or coverage of building RVMAT dependencies. A second selected
stump model found the PAA already resident and used ordinary registration.

The route's `drw:init` mean/p95/max was 14.002/99.526/656.458 ms. It had
extra warm/provenance instrumentation and a different asynchronous/cache
history from the earlier packed-only/combined routes, so those values do not
establish a performance gain or no-regression. This path covers one shared
tree stage, while the `city_school`, `village_policestation` and
`tenement_small` first-touch stalls remain unsolved. Keep the feature off by
default; `performanceAccepted=false` and `defaultReady=false`.

## Two-phase building raP pilot: runtime source-binding gate not exercised

Commit `e6a0f501` adds a separate default-off pilot for one authored Multi
NormalMap in each of `tenement_small`, `city_school` and
`village_policestation`. The worker parses one exact physically captured raP
before conversion; an intermediate `RapCapture` state lets the owner recheck
the currently mounted material and capture the PAA source; then the worker
resumes conversion. Refusal, stale work, reset and failed queueing retain the
ordinary complete-model fallback. Other material layers and proxy models are
uncovered. A review removed a process-lifetime two-attempt callback cap that
would have prevented the third candidate even when source-binding capacity
became available. Synthetic mapped-PBO tests passed 79 assertions across
successful preparation, refusal, stale cancellation and reset; the tree test
still passed 19. The paired build and installed WGPU startup passed.

`Test-DayZStreamingLatency.ps1 -RapBuildingPilot` sets the exact warm/source
options and requires a same-named building source capture and source-validated
GPU upload. The installed `e6a0f501` route
`build/stream-residency/rap-buildings-on-sep29-disabled-20260929-163310`
ended the game normally with 912 frames, zero dropped samples and full
19,923/19,923 returned residency, but the harness correctly reported
`passed=false`: all three worker raP candidates were found, all three owner
captures logged `sourceBound=false`, and no building prepared texture was
taken/uploaded. The sole warm worker Put was from another stage and was never
taken. Final archive-source inventory was 256/256 live bindings with 4,612
cumulative capacity refusals. Those global totals do not prove the exact
refusal reason for each of the three captures; earlier local DayZ probes have
already shown exhausted material-preflight binding reservations. First-touch
rows here were 516.2 ms/81 uploads for tenement, 940.7 ms/118 for school and
283.0 ms/43 for police. They are not a matched performance comparison.

Do not promote or infer a building-performance benefit. The next source seam
should examine whether an immutable physical PBO member can be prepared on a
worker and validated against the current mount at Take/upload without
retaining one of the scarce initialized texture bindings. Preserve exact
identity and complete ordinary admission; do not raise the binding cap on
these aggregate counts alone. All related flags remain off by default;
`performanceAccepted=false`, `defaultReady=false`.

## Actual DayZ first-touch upload inventory

Commit `18c358c8` adds a default-off, owner-only `WGR_OBJECT_STREAM_DAYZ_TEXTURE_TRACE=1`
diagnostic. While the three selected DayZ buildings are first admitted, it
records the slowest 16 *actually reached* texture uploads per model, including
proxies. Fixed top-K storage is allocated only for those model names. It is a
source-read-cost inventory, not a material-slot attribution or GPU-time
measurement; the default path does not perform the scans or logs.

The paired installed build passed WGPU startup. A single locked DayZ route,
`build/stream-residency/dayz-texture-trace-sep29-disabled-20260929-171445`,
passed and exited normally (916 frames, zero dropped samples). Tenement,
school and police reached 81, 119 and 43 uploads; measured aggregate read
work was 359.0, 563.7 and 243.6 ms respectively. The most expensive tenement
source was `plaster_flats02_nohq.paa` at 20.264 ms; school started with
`school_doors_nohq.paa` at 21.830 ms and police with
`police_station_wall_int_nohq.paa` at 20.584 ms. Each also has many other
10-20 ms sources. This explains why a single prepared NormalMap cannot remove
the first-touch hitch. Any broader worker prefetch should select sources
proven to be consumed, respect material/source identity and the existing byte
budget, and still measure matched performance before promotion.

## Binding-free physical-member building pilot

Commit `4e04f796` keeps the same three-building, default-off raP pilot but
replaces its unsuccessful initialized-source-binding capture with an exact
physical PBO member request. The owner rechecks the authored material and
captures one current NormalMap member, at most 16 MiB, before conversion. The
worker reads/prepares it under the existing publication and byte limits. Only
these three PAA names can acquire a numeric birth identity from the precise
mapped buffer used by `TextureSourcePac::Init`, including a cached buffer;
loose, compressed, failed or unknown sources fall back. At Take, the store
requires that Init identity to match the worker request. Before upload WGPU
also checks that the same physical member is still mounted and that every mip
matches the parsed header. No scarce `ArchiveSourceBinding` is retained for
this path; the old bound tree pilot is unchanged. Its explicit environment
switch is `WGR_OBJECT_STREAM_RAP_BUILDING_PILOT=1`.

The paired game/test build and 67 focused assertions across three cases passed;
the archive-member test also passed 25 assertions in a separate process with
the source-binding-capacity options active. The committed, pushed EXE/DLL pair
passed installed WGPU startup. One locked installed DayZ route,
`build/stream-residency/rap-physical-sep29-disabled-20260929-165746`, passed
the updated screening harness: normal exit, 909 frames, zero dropped samples,
and three same-named captured/current/validated uploads. Tenement uploaded
5,592,064 prepared bytes, school 1,398,016, and police 1,398,016. Their
individual cold model admissions still cost 500.1 ms/81 uploads, 915.2 ms/118
uploads, and 295.8 ms/43 uploads, with only one prepared upload each.

One same-build control route with material-preflight/provenance flags but the
building pilot off,
`build/stream-residency/rap-physical-control-sep29-disabled-20260929-165952`,
exited normally with 1,031 frames and no dropped samples. Its harness result
was `passed=false` only because the warm-upload requirement was not exercised.
Its `drw:init` mean/p95/max was 10.342/65.017/760.418 ms versus the pilot's
14.570/102.151/686.906 ms; cold admissions for the same three buildings were
329.5/608.1/189.6 ms. These sequential runs differ in OS cache and async
history, and their one-of-many upload coverage is too small to explain the
different totals. Thus the source-safe handoff is demonstrated, while
performance improvement and no-regression are *not* established. The pilot
stays off by default; `performanceAccepted=false`, `defaultReady=false`.

## Bounded tenement material-stage pilot

Commit `f7d75802` adds a separate default-off
`WGR_OBJECT_STREAM_RAP_BUILDING_MULTISTAGE=1` extension to the physical-member
building pilot. For `tenement_small` only, the owner captures at most 16 authored
visual-LOD raP material members; the worker identifies up to four schema-typed
NormalMap/LayerNormal stages that were observed in the prior actual-upload
trace; the owner revalidates each current material/PAA member before worker
preparation. The existing publication, byte budget, stale/reset fallback and
exact Init/current-mount/mip upload checks still apply. The single-stage school
and police pilots are unchanged. Without the flag there is no extra material
scan or texture work.

The paired build and focused tests passed (111 assertions for raP names and
multistage handoff; 90 for the old physical store/building pilot), followed by
the pushed, installed EXE/DLL pair and WGPU startup check. One locked DayZ
route, `build/stream-residency/rap-multistage-sep29-disabled-20260929-174520`,
passed and exited normally (805 frames, zero dropped samples). It discovered
four tenement candidates, captured all four physical PAA members and logged
four validated GPU uploads of 5,592,064 bytes each. School and police retained
their one validated physical upload apiece. Yet tenement first-touch was
517.0 ms with 81 uploads, school 1,111.2 ms/118 and police 552.2 ms/43.
This route has a different OS cache and async history from previous runs;
neither its totals nor the four uploads prove a latency gain or regression.
Most texture work and geometry optimization remain on the owner path.
`performanceAccepted=false` and `defaultReady=false`; keep the pilot off.

## Shape-install cost split: registration dominates vertex-buffer optimization

Commit `306abcfd` adds a default-off, exact `tenement_small` diagnostic via
`WGR_OBJECT_STREAM_DAYZ_SHAPE_SPLIT=1`. The existing cold-model `opt` timer
encloses both `WorldShapeLoaded` (early retained model registration and texture
resolution) and `OptimizeOneShape` (legacy vertex-buffer work). The new split
does not skip or defer either call, and it adds no clocks or logging to the
default path. The paired build, pushed installed EXE/DLL and WGPU startup
passed. A single locked DayZ route,
`build/stream-residency/dayz-shape-split-sep29-disabled-20260929-180019`,
passed and exited normally (754 frames, no dropped samples). Its exact cold
tenement was 685.4 ms/81 uploads; texture read was 474.7 ms. Of the 398.2 ms
inside `opt`, `WorldShapeLoaded` took 396.709 ms and legacy
`OptimizeOneShape` only 1.496 ms. The texture aggregate includes nested proxy
work and overlaps that `opt` interval, so the buckets must not be added.

Do not bypass `OptimizeOneShape`: late/partial coverage still needs its vertex
buffers. The bottleneck is early owner-side model registration and texture
first touch. The different cache/async history of this run prevents a matched
regression claim. A useful next implementation must reduce or distribute that
owner work without publishing incomplete model state; the default remains
unchanged and no GeometryPage or DayZ performance acceptance follows.

## Exact owner primary-texture staging: safe handoff, insufficient coverage

Commit `2e5d9f76` adds default-off
`WGR_OBJECT_STREAM_DAYZ_OWNER_PRIMARY_STAGE=1` for the exact
`tenement_small.p3d` model. One converted shape remains outside ShapeBank;
one initialized primary section texture is uploaded per owner residency update.
The slot is limited to one model, at most 64 distinct primary textures and a
128 MiB conversion-size estimate. Cancellation releases the slot if its
originating placement leaves the desired set. The ordinary complete
`NewFromModel`/`ObjectCreate` path runs only after staging; material-stage and
proxy textures still follow that path. No partial model is published.

The paired game/test build, safety push and installed WGPU startup passed. One
locked DayZ route,
`build/stream-residency/dayz-owner-primary-sep29-disabled-20260929-183133`,
passed and exited normally (687 frames, zero dropped samples, no forced exit).
It staged seven primary uploads over seven updates, totaling 114.095 ms; the
largest individual step was 37.654 ms. The same shape then admitted successfully
with zero staged textures lost before registration, but final creation still
took 586.831 ms. The remaining material/proxy work dominates. This is a
correctness and handoff screen, not a matched FPS result or a frame-time
deadline. Keep the flag off; `performanceAccepted=false`,
`defaultReady=false`. The next useful slice needs measured, current-source
coverage of the actually consumed material/proxy textures or a complete
registration strategy that does not expose partial model state.
