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
