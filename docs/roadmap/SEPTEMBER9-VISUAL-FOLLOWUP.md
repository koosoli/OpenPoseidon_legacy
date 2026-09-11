# September 9 visual follow-up

## September 11 native texture identity optimization

[Measured native bank costs and canonical texture reuse](decisions/NATIVE-TEXTURE-IDENTITY-20260911.md):
e1738ab4 installed. Same-binary route A/B passes lifecycle and identical endpoint
instance/geometry counts. Native archive path aliases now reuse the existing
texture object rather than allocating duplicate sources/uploads. Process peak
falls ~0.56 GiB, tracked object textures ~248.5 MiB in this pair. No stable FPS
or stutter-elimination claim; unique texture preparation remains expensive.

## September 11 upstream recheck

[Fresh upstream comparison](decisions/UPSTREAM-REVIEW-20260911.md): remote main
remains 3541affa, no new commits since September 8. Cursor geometry/hotspot
adaptation is the clearest missing correctness candidate. Opaque bucketing needs
a fork-specific benchmark; existing sort/filter optimizations are not absent.
Preloader reset and destruction-instancing guards already exist. No blind merge
or GL33-only changes were made during this review.

## September 11 tester snow/terrain report

[Snow tester follow-up](decisions/SNOW-TESTER-FOLLOWUP-20260911.md) separates the
reported working storm/indoor mask/crawl/refill/craters from deep-snow, rotor-wash
and post-edit player-visibility defects. Filled snow chunks previously remained
allocated forever until the hard cap blocked new trails. Bounded reclamation is
implemented and unit-tested without evicting live depressions; installed
0a76682f refill/crawl acceptance passes (crawl-20260911-031116, chunks 8 -> 0 -> 5,
visible renewed trail without reset, clean exit). Other tester issues remain open, not folded
into that storage fix.

## September 11: complete-window installed acceptance

Installed c22aa78b, native run cancel-20260911-024613-slow-trace-bc7-default-driver-memory-tint-reuse-0
completes cleanly in 135.7 s. Strict parser accepts 288 static and 1296 moving
records, zero dropped frames. Moving median/p95/p99/max are
30.511/43.418/91.965/220.947 ms; seven frames exceed 100 ms. The old log-delta
analysis counts six, demonstrating why the separate full-window data matters.
A units error in the first REPORT (milliseconds passed to a seconds API) is
corrected and pinned by a Python test; original engine log values are valid.
Six parser/report tests and 52 native unit cases / 1583 assertions pass.

The worst frame (838) attributes 216.244 ms to residency. Immediately preceding
bank-table preparation is 189.144 ms for five models, plus 26.0 ms cold-model
installation. Whole-route sim:step max is 9.573 ms. This route's largest stall is
not an argument for broad AI threading. The earlier count cap still failed its
coverage gate, and full DDS preparation still has no stable timing win; inspect
individual table work rather than silently enabling either rejected experiment.

## September 11: installed cancellation check and complete profiler windows

124b6ee3 is installed; cancel-20260911-023206-slow-trace-bc7-default-driver-memory-tint-reuse-0-async-dds
passes strict lifecycle checks in 135.5 s, 22370 instances. New JSON fields show
905 DDS puts / 681 takes / 0 configuration misses; the 3396.6 MiB put counter is
cumulative traffic, NOT residency (471.4 MiB at capture). This verifies the
increment, not a sustained FPS win or a fix of the intermittent DDTerm hang.

Next measurement increment extends the EXISTING FrameProfiler with an optional
16384-frame bounded window (<1 MiB records), allocated only on explicit start.
No per-frame I/O/allocation, no replacement scheduler/profiler, rolling 256-frame
UI stats unchanged. triPerfCapture starts at the next BeginFrame and stops before
printing chunks/phase summaries; partial boundary frames are excluded. Overflow
and truncated/duplicate/missing chunks are failures, never silently averaged.
The camera harness records static and moving windows. These measure completed
World main-loop frames, NOT GPU duration or time outside BeginFrame/EndFrame.
Installed acceptance is pending; older timestamp traces retain their limitations.

## September 11: DDS timing repeats do not justify default activation

Same installed d8c931d5, corrected version-3 native/MSAA4 profile and identical
4 km route: valid OFF runs 015814/022352 yielded 32.3/32.3 moving throughput FPS,
p99 94/92 ms; valid ON runs 021208/022033 yielded 33.1/32.0 FPS, p99 98/96 ms.
Peak private bytes rose from about 10.98/11.01 GiB to 11.64/11.60 GiB. Sampling
is 1 Hz; trace tails have 95.6-96.2% one-to-one coverage, not exact frame tails.
No sustained improvement established. The earlier ON shutdown timeout remains
unresolved; two clean repeats do not prove it fixed. Keep DDS preparation OFF.

Next bounded improvement stops optional texture work between files when the
active model request is camera-stale, stopped or from an earlier world. The queue
mutex is only held for the check, never I/O, decoding or store calls. The existing
cancellation fixture now carries a real texture reference and verifies zero
attempts for discarded requests versus one for a camera-revived request. Combined
PAK/DDS/PAA/preparer suite passes 51 cases / 969 assertions. Capture JSON now
includes DDS-specific puts/takes/configuration misses; old puts/takes count only
the original payload path. Installed regression for this increment is pending.

## September 11: native DDS byte oracle passes, shutdown gate failed

Installed d8c931d5, pack 0.1.4. Clean oracle capture
cancel-20260911-015523-slow-trace-dds-prep-bc7-default-driver-memory-tint-reuse-0-async-dds-verify-dds
completed shutdown and verified 671 consumed DDS sources exactly (375 tinted,
95 coverage composites; categories overlap). 134.7 s, 22370 instances. An earlier
667-source oracle had a +dirty harness stamp and is diagnostic only.
The same-build OFF timing control cancel-20260911-015814-slow-trace-bc7-default-driver-memory-tint-reuse-0
passed, but ON run cancel-20260911-020051-slow-trace-bc7-default-driver-memory-tint-reuse-0-async-dds
hung in DDTerm after sun/moon loading and hit the 290 s watchdog. It is INVALID
for acceptance, despite having an image/metrics file. Similar DDTerm hangs existed
before this feature; no causal attribution yet. Experimental preparation remains
OFF by default. The manual debugger attach did not obtain usable stacks before
timeout. The harness now saves a MiniDumpNormal of its owned process before
timeout termination, not a full-memory dump. PS5.1 ownership/signature/overwrite
fixtures pass (~605 KB); LLDB successfully reads a separate fixture's stacks.
Next: obtain the actual hang dump and diagnose it before claiming pipeline acceptance.

## September 11: native DDS worker experiment awaiting runtime acceptance

After 185a9c4a, the existing native model workers and DDS factory are connected
through the completed-source hand-off, gated by WGR_NATIVE_DDS_PREPARE=1 (OFF
until acceptance). No new queue or upload thread. CPU composition uses owned
archive requests, mount/store generations and one options snapshot. Exceptions
in optional texture preparation preserve the parsed model and fallback path.
Mount lookup/remount now use a shared mutex; I/O/decompression happen after
releasing it. Root returns an owned string. Synthetic remount/read tests and
the enabled factory hand-off pass: 51 combined cases / 965 assertions.
Runtime byte verification and same-build A/B are pending, not claimed passed.

## September 11: DDS hand-off contract, integration pending

61b18e73 is installed and its native moving-camera regression completed cleanly
with the corrected uncapped graphics profile. The DDS options are frozen per
composition and tested across a live tint switch. The next increment adds an
owned completed-DDS payload to the existing PreparedTextureStore, sharing byte
budget, TTL, upload marks and Clear generation. It is not yet called by the
worker or texture factory: no asynchronous DDS performance claim. Unit cases
cover identity/pixels, stale publication, configuration changes, duplicates,
upload/eviction, 1 MiB rejection, short TTL and disabled preparation. Full
EDDS/PAA/preparer suite: 48 cases / 903 assertions. Next: connect the existing
worker and factory behind a measured A/B, respecting archive lifetime and
containing preparation failures; do not add a competing streaming manager.

## September 11: benchmark migration correction and static DLSS check

The unversioned generated graphics profiles migrated fpsCap=0 to 144 on this
machine. Earlier captures must NOT be described as uncapped. Their low native
Reforger FPS does not prove the cap had no effect. The material, AI-scale,
modernisation-CPU and native cancellation harnesses now share an explicit
version-3 Ultra profile, refusing to overwrite existing profiles. PS5.1 fixture
checks cover four upscaler modes, overwrite refusal and missing directories.
Installed 872bf0ee sand closeups with pack 0.1.4 passed strict lifecycle checks:
material-20260911-010400-Sand-Full-h17.5-d1-close-dlss (28.1 s), and
material-20260911-010912-Sand-Full-h17.5-d1-close (26.3 s). Fine relief is visible
in both inspected images. This is static visual evidence, not moving-camera
ghosting acceptance or a measured performance improvement.

## September 11: streaming cancellation and DDS reader boundary

Installed 872bf0ee contains e200c818's unlocked queued-job retirement during Reset
and generation-checked PAA publication after Clear. Synthetic tests cover both
interleavings; 22 preparer/PAA cases pass normally (391 assertions) and with eight
workers plus concurrent adaptation (1242). DDS composition now accepts an owned
read callback; normal callers retain the existing reader. The combined EDDS/PAA/
preparer suite passes 44 cases / 827 assertions. This is a prerequisite, NOT a
completed native asynchronous material pipeline. Native installed captures on
both commits complete shutdown with 22370 registered instances. Details and
remaining configuration/lifetime requirements are in the three-track decision.

## September 11: default sand detail, acceptance still incomplete

c4bbb2b3 is installed with own material pack 0.1.4. One optional world-space
detail NOHQ layer gives the mapped Everon sand a 0.5 m fine-relief period in
addition to its 50 m macro map. The existing terrain/bank pipeline is extended;
original, authored A3 and native Enfusion materials get no detail layer. Sampling
fades at 15..35 m and the far Satmap early-out remains intact. Normal and close
installed A/B captures complete shutdown and show a close-range detail effect.
Eight generator tests, four terrain contract tests and shader composition pass.
Photorealism, roughness binding, motion/DLSS/snow and full package acceptance
remain open. See DEFAULT-CONTENT-PACKS-20260910.md for exact captures and limits.
The hardened capture validator has 11 fixture tests; missing lifecycle flags no
longer silently pass. This is not a fix claim for the intermittent native hang.
Native regression capture cancel-20260911-002533-slow-trace-bc7-default-driver-memory-tint-reuse-0
on c4bbb2b3 also completes shutdown: 134.7 s wall, 22370 final registered instances,
134 whole-GPU driver-memory samples. It ends over the ocean (0 main objects),
so this is loading/lifecycle evidence, not native village visual acceptance.

## Texture precision and measured DDS preparation, September 10 afternoon

AI88 precision defect fixed in 9eccd411: PAADecoder used PacARGB4444 as an
intermediate for AI88, discarding four bits of both intensity and alpha. Both
buffer and selected-file-mip paths now preserve the original AI88 bytes before
expanding to RGBA8. No change to GPU format, texture dimensions or DXT upload.
Synthetic all-256-level test failed on the original code (1 decoded as 0), then
passed; decoder/streaming suite: 21 cases / 5249 assertions. Installed-game Field
smoke build/modernisation-cpu/cpu-20260910-160814-Field exited 0, no renderer errors,
image inspected. This is NOT a matched screenshot proof for the user's road/RPG
sight: their asset identity and original capture settings remain unconfirmed.

a39dd881 adds opt-in POSEIDON_DDS_PREP_TRACE=1 to distinguish DDS read, decode,
coverage read/decode/merge, tint and finalisation. Timings include CPU preparation
and waits, not GPU upload; only calls >=2 ms are logged. inputBytes is file data,
outputBytes the retained mip payload, neither is peak process RAM/driver VRAM.
Native route cancel-20260910-161933-slow-trace-legacy-tables-dds-prep completed,
exit 0, 22370 final registered instances. 1353 logged calls: total 22126.661 ms,
read 4752.597, decode 15344.360, merge 393.131, tint 1192.078. Cumulative, not a
22-second frame; thresholds exclude short calls. The ending image is ocean.

f79ac2a9 adds an OFF-by-default BC7 block-row batching experiment on the existing
TaskPool, not a scheduler/AI rewrite. POSEIDON_DDS_BC7_BATCH=1 selects it; 2 also
compares every eligible mip against serial output in the running game. Only
main-thread, BC7, >=256x256 decode calls qualify; external preparer threads and
small mips remain serial. Owned output ranges join before tint/bank/GPU use.
Unit checks include all eight modes, invalid blocks, odd edges, canaries, and
1/2/4 pool threads: combined BC/EDDS/PAA suite 46 cases / 9829 assertions passed.
Runtime oracle: cancel-20260910-163111-slow-trace-legacy-tables-dds-prep-bc7-2,
1882 eligible mips byte-identical, exit 0, no mismatches. Three subsequent pairs
on f79ac2a9 reduced exact logged moving maxima 277.286/254.484/275.880 ms to
162.390/165.841/183.793 ms. No consistent FPS/p99 improvement or memory saving;
the accepted benefit is shorter loading spikes without reducing texture data.
BC7 batching now defaults ON; explicit 0 is the serial control, 2 the oracle.
The complete run table and remaining gates are in the three-track assessment.
Final default installed as ae8c90fa at 16:53. Native route
cancel-20260910-165344-legacy-tables-dds-prep-bc7-default (no BC7 override) completed,
exit 0, no contention/timeout/renderer failures; 22370 registered instances.
Image inspected: ocean endpoint, not a distant-settlement quality acceptance.
Final combined BC/EDDS/DDS/PAA/preparer suite: 81 cases / 10314 assertions;
additional alpha tag: 3 cases / 6 assertions. Cargo.lock SHA256 remains
9A4E74D791B26A4303E064867DA67B862C1439D0635DF3E19ED5A97AADF31A2E.

## Three-track audit and streaming lifecycle, September 10

See decisions/MODERNISATION-THREE-TRACKS-20260910.md for the requested code-based
assessment. Baseline is safety/consolidated-v2 at 2ad0a9be, not old chat claims.
Implementation was subsequently authorised. Priority is existing loading-path
completion, a bounded far-LOD experiment, then measured CPU candidates; no new
scheduler, wholesale AI rewrite, or duplicate streaming manager.

ObjectStreamPreparer now drops same-world stale active parse/conversion results
on completion and removes stale queued conversions. A returning desired set can
revive active work; in-flight slots are not reused early. Synthetic blocked-worker
tests exercise stale completion, queued conversion ownership and return-to-window.
PoseidonTests [preparer],[physics]: 66 cases / 2436 assertions pass; the shared
test build also includes the previously unfinished sparse terrain journal, with
its missing test include supplied. This does not lift the Fusion editing limit.
Cargo.lock is unchanged. Installation 2ad0a9be+dirty at 13:44 includes those local
terrain changes too; it must not be attributed to a clean remote commit.

Installed native Everon 4 km / 100 m/s camera route:
build/native-stream-cancellation/cancel-20260910-134449. Exit 0, no renderer errors,
40 waypoints completed, stale counter reaches 4. Final image inspected: camera
has reached the ocean, so its 18.57 ms GPU sample says nothing about village FPS.
This is lifecycle coverage, not an A/B performance win or full cancellation proof
for all active conversion races / multiplayer.

The route exposed two measurement bugs: farfield-frametimes.py missed the newer
desired/lead residency fields and counted duplicate renderer-frame IDs as extra
frames. Both corrected, four Python regression tests pass. Corrected moving trace:
1335 frames / 55.0 s, median 28 ms, p95 41 ms, p99 692 ms, 35 frames over 100 ms;
93.7% counter coverage, so multi-frame averaging limits tail precision. Actual
FrameProfiler final ring: p99 662.75 ms. The prior uncorrected 43.6 FPS throughput
was false; corrected trace is 24.3. This is one diagnostic run, not a controlled
performance improvement. Recurring native clutter rebuilds cost 319-359 ms even
though the same 340109 grass cells / 37726 road exclusions recur. An existing-path
definition-reuse experiment was subsequently accepted, within the limits below.

Native clutter follow-up, installed 2ad0a9be+dirty at 14:00: geography-only refreshes
reuse existing surface runs/atlas, retaining dynamic road/building geography work.
World/material/UI uploads rebuild. No additional atlas allocation or GPU memory.
39 byte-identical reference comparisons passed in the installed verifier. Three
alternating same-binary native route pairs reduced moving p99 from 356/370/381 ms
to 86/101/187 ms; periodic clutter bake now 7-10 ms instead of roughly 320-360 ms.
All six routes completed with the same 22370 registered instances and 1923.552 MiB
tracked GPU residency. RAM peaks are essentially unchanged. The third reuse run
still contains a 988 ms moving outlier, so not all streaming hitches are fixed.
Full run IDs, controls, memory values, trace caveats and remaining gates are in
the three-track decision. An optional one-second process-memory sampler was added
to the existing benchmark harness, not to the game loop. Four Python trace tests
and PowerShell syntax checks pass. No AI parallelism or HLOD was implemented here.

CPU follow-up: two current repeats each of sparse Field, idle 80/320 and combat
are now recorded in the decision. Idle320 AI averages 0.582/0.575 ms/tick over
the last ten samples; combat 1.882/2.162 ms/tick, with vehicle/man simulation
3.573/3.729 ms/tick. Do not confuse those with per-frame render waits. Campaign,
Fusion traversal, exclusive animation and driver-VRAM timelines remain open.
No additional AI workers are justified by the current evidence alone.

Sparse terrain undo follow-up: first-write 16x16 sample tiles replace the initial
whole-world backup. Overlapping edits retain original heights; bounded admission
refuses over-budget regions before reading them. Reset still constructs/publishes
a whole collision grid, and the 2049-range guard remains: this is NOT the Fusion
editor fix or a large-map editing claim. Installed rocket/reset acceptance
build/rocket-craters/acceptance-20260910-144704 passes: height 29.835 -> 28.835,
disabled stays 28.835, defaults restored -> 27.835, terrain restore -> 29.835.
Game exits normally, no renderer errors. Local-edit memory/work reduction is
structural; no FPS improvement claimed. Existing journal unit tests pass as part
of the 66-case run above. Harness outputs are now timestamped to preserve evidence.

Native bank-table follow-up: individual slow-frame traces isolate the uncharged
main-thread BuildAdapterBankTables stage before the existing cold-install counter.
An independent two-table budget is implemented but DEFAULT OFF after three pairs:
moving maxima improve from 265/294/297 to 163/153/167 ms, while static-phase p99
worsens and initial residency is 310-403 objects behind. By movement start/end
coverage is equal; this does not excuse a changed initial view or prove higher FPS.
See the decision for all six run IDs, memory, tails and the opt-in control
WGR_OBJECT_STREAM_TABLE_BATCH=2. Default remains 0. Individual native material
preparation, byte budgets, cluster HLOD and complete workload coverage remain open.

Measurement safety: the trace analyzer now has five passing regression tests,
including why gap-averaged p99 is not a formal lower bound. Benchmark cleanup
retains original Process handles and tests HasExited rather than looking up old
PIDs, and its standalone foreign-process check includes OpenPoseidon. The outer
game-folder lock remains mandatory. No foreign Cargo.lock or temporary directories
were changed or removed.

Final installed regression check: 058cc4d4, matching EXE/DLL deployed at 15:42.
PoseidonGame and PoseidonTests build; 66 preparer/physics cases, 2436 assertions,
pass. Field smoke cpu-20260910-154258-Field exits 0, no contention/watchdog, 52507
registered / 10576 main instances and 650835 main triangles; the repaired harness
emits no leftover cleanup warning. Rocket/reset acceptance-20260910-154404 again
restores 29.835 exactly after two 1m craters; screenshot inspected, exit 0, no
renderer errors. No additional FPS or visual-quality claim from these smoke tests.
The five Python trace tests pass. All runtime changes are committed/pushed; only
the foreign Cargo.lock and existing untracked temporary directories remain dirty.

## Bounded crown flutter and spray motion, September 10, 13:10

The additional cutout-only flutter previously scaled with wind, height response
and gust, independently of the solid trunk. At amp=1.05, hs=5, gust=1.65 its
per-axis amplitude was 1.906 m before the leaf slider. It is now capped at
0.025 m times that slider (default 1), while common trunk/crown bending is
unchanged. Both direct and retained motion-vector formulas use the identical
cap, including previous-frame evaluation. No texture, extra draw or resource
allocation; two contract/numeric tests and six shader/composition tests pass.

Installed 5e25c3ad+dirty at 13:04, native Reforger Forest pose with 18m/s wind:
Forest-wind-uncapped-1257 / Forest-wind-bounded-1303 under build/native-reforger-perf.
Both loaded 22360 instances, completed normally, no renderer errors. Inspected
both forest images: trees remain present and retain their normal common sway;
this bounds the section separation, not a claim of authored skeletal wind or
complete attachment continuity for every tree. Whole-frame samples 23.34 / 22.76ms
are not a demonstrated performance gain (triangles differ slightly with culling).
Native material flatness, model-space pivot restoration and imported branch
weights are separate work, not closed by this small deformation correction.

The existing world-anchored spray seed (21a6570f) now has an installed moving
camera smoke test: scripts/Test-WaterSprayMotion.ps1, native, 2m/s across the
0.9375m spawn lattice, wave profile 2.2 / wind18 confirmed. Run
build/water-spray-motion/spray-20260910-125301 exits 0 with moving-phase traces
and no renderer errors. Screenshot inspected, ocean and whitecaps present.
This is runtime/motion coverage, not proof that all foam grain or temporal
coastline vibration is visually fixed. The coarse painted appearance of strong
whitecap coverage remains open; the rejected bubble-cell experiment stays off.

## Pub physics diagnosis, September 10, 12:50 (not a collision-fix acceptance)

Added scripts/Test-PubPhysics.ps1, a bounded dev_physics_probe harness command,
and opt-in POSEIDON_PHYSICS_COMPONENT_TRACE=<model-name-fragment>. The command
uses engine XYZ metres, caps test probes at 32 and launches at 20 m/s; it does
not change normal gameplay. Component tracing runs only when explicitly enabled.

The first hypothesis, missing world-list houses in PhysicsCorpus, was falsified
on current Everon. A temporary extra-building walk registered ZERO extra bodies:
all 49689 registered scenery bodies already included the pub. Both paths returned
the same four horizontal hit distances (2.422, 4.135, 7.424, 2.370 m). A sphere
launched west from engine [5092.16,18.34,3994.46] stopped inside at x5090.119.
The redundant production change was removed; do not reintroduce it as a fix.
The first control used a nonexistent shutdown harness command; cleanup killed
only its own process. The subsequent run used exit, returned 0, no renderer errors.

The separate -Furniture diagnostic enables LooseObjects and disturbs the actual
nearest TableHospoda, without first populating the whole-world corpus. It adds
24 nearby static bodies in 2.5 ms, so the 4000-body cap was NOT reached here.
The table is caught horizontally but its origin drops from y16.4975 to about
y15.75 while tipping; this is a reproduced unresolved furniture/floor defect.
Do not read the script completing its diagnostic capture as a passing furniture
containment assertion. Installed 58f8f9df+dirty at 12:43, evidence under
build/pub-physics/furniture-20260910-124344; the sphere run is
build/pub-physics/buildings-20260910-122554 (temporary diagnostic naming).

The authored solid house component 42 spans [5089.130,16.358,3986.706] to
[5091.810,18.223,3994.074], overlapping the initial table region
[5089.176,16.496,3992.493] to [5090.515,17.505,3993.810]. This coarse furniture
region remains in the static house when the visible proxy becomes a Thing.
Roadway LOD floor surfaces have no convex components and are not yet registered
as physical triangle surfaces. Full fix needs faithful static surface collision
and a reliable ownership split for promoted furniture, not globally disabling
house hulls, inflating CCD flags, or enabling every prop by default. Preserve
original mission/MP/save behaviour until those ownership gates are exercised.

## Smoke contact retention and shared sweep candidates, September 10, 12:15

A tangential particle sweep no longer discards a nearby roof/wall clipping plane
unconditionally. A short surface probe confirms that geometry still exists;
leaving its edge or removing the roof releases the plane. Offset sweep rays
report centre-line hits, so confirmation includes the particle radius. The new
unit case covers contact retention, roof edges, removal and distant particles.
All 14 [smoke] cases / 442 assertions pass after the final build.

The seven rays of a sweep now reuse one bounded, stack-allocated candidate list.
Per-ray bounds and original cell/list order remain unchanged; over 32 overlapping
candidates falls back to the original full queries, never truncating collisions.
POSEIDON_SMOKE_BATCH_VERIFY compares exact hit buffers with the original path.
Both Showcase and Nogova installed 30-second runs complete more than 1000 exact
comparisons with no mismatch, no renderer errors and exit 0.

Installed 5d3ba993+dirty at 11:59: build/smoke-cold-grid/roof-batch-cost and
roof-batch-control use the same executable with batching on / broadphase off.
At about 3900 particles the Showcase late simulation samples are commonly
7.25-7.43 ms (outliers up to 10.95), versus 8.67-9.29 ms in the control.
The extra contact confirmation alone cost roughly 2 ms before batching;
this is not a claim of a whole-frame FPS improvement. Whole-frame measurements
actually vary 18.35 vs 17.33 ms and are unsuitable for that conclusion.
Nogova production roof-batch-nogova-cost has late samples 1.18-1.49 ms,
about 3760 particles tracked indoors, and the same roughly 3900 total particles.
No particle budget, wind coupling or downwash response was reduced.

Inspected both final exterior captures: no plume appears above the closed roofs.
The original owner-reported roof leak was not reproduced in either baseline,
so these checks do not close every thin-roof smoke defect. The contact lifecycle
fix is independently demonstrated by its regression test; performance evidence
is scene-specific, not a promise for all smoke loads or GPU costs.

## Breakable standalone glass, September 10, 11:25

LabPanelGlass now has armor=1 and the opt-in Building config property
removeOnDestruction=1. At destruction, the local owner queues normal entity
removal instead of the house-collapse morph; renderer and collision retire
together. Other classes default false and use the original Destroy method.
This is whole-pane breakage, not a glass-shard simulation or automatic conversion
of every window embedded in an existing building. Multiplayer/save reload after
breakage has not been exercised, so no compatibility acceptance claim there.

Installed c195a2d5+dirty at 11:19 and regenerated Showcase addon: real
BulletSingleW projectiles remove labGlass; frame posts and the normal-map control
survive. Before impact, walking stops below z3504; afterwards the player passes
z3504 alive. scripts/Test-Showcase.ps1 -GlassOnly passes twice, exit 0, no renderer
errors. Four deterministic asset tests and five Object cases / 22 assertions pass.

Rear-side control is now explicit. The first 2m camera clipped even the frame and
was rejected; an 8m reverse view with the opaque control panel moved aside shows
the pane's tint. Same-run intact/broken RGB at (640,450): (96,92,65) -> (107,103,74);
outside controls (900,450) and (400,450) remain exactly unchanged. The pane is
drawn on both sides in this setup, but weak rear sheen remains a material-quality
limitation, not a fixed global backface-culling defect. No culling flag changed.

## Weather access and rocket defaults, September 10, 10:55

Snow is now a default-open Weather section, without the experimental-only header;
cloud/fog, wind, rain and wind diagnostics have separate collapsible sections.
The snow section retains the actual limitations (mission-local, no save/network
or altered ground collision), exposes live/drawn flake counts, and explains that
Snowstorm deposits 10 cm/min rather than instantly covering the terrain. Its
preset is shared with the installed test harness; it does not force wind or rain.

Installed 21a6570f+dirty at 10:50: Snowstorm increased active flakes from 209 to
5585 and submitted flakes from 83 to 2270 within three seconds; cover grew from
0.18050 to 0.18570 m. Normal/storm captures inspected at fixed exposure 1.0.
An earlier exposure-0.15/reset run was visibly too dark for visual acceptance,
although particle counters passed. This verifies the preset, not a fully accepted
blizzard look or the owner's roof/moving-camera defects. Snow crawl and master
reset regressions pass; 10 native Snow cases / 9923 assertions pass.

Rocket craters now default ON, explicitly requested by owner. SP/land/contact
and supported-grid guards remain. Installed scripts/Test-RocketCraters.ps1
confirms default-on, off, settings-reset-on and original-terrain restoration:
29.835 -> 28.835 -> 28.835 -> 27.835 -> 29.835 m, exit 0, no renderer errors.
This is not acceptance of large-grid Fusion editing or a measured salvo-cost
claim; first-contact collision preparation can still pause on supported grids.
Cargo.lock remains untouched. New immutable backup before this work:
codex/backup-asset-obs-20260910-101222 at 21a6570fc8b9085671b77c695cead9881f18b05a.

## Owner follow-up and backup checkpoint, September 10, 10:12

New reports below are OPEN, not covered by earlier passing captures. Work order
remains small verifiable corrections before structural changes. Owner requests
a new immutable GitHub backup first, then continued implementation.

| Area | New acceptance target / reproduction |
|---|---|
| Native Reforger wind | Trunks and crowns must not move independently at their attachment. |
| Native forest/ground | Remaining flat material appearance; repair authored channels, not global contrast. |
| Native streaming | Profile fast flight and model admission spikes, not only settled-camera FPS. |
| Native interiors | Odd textures at freefly [5095.96,3948.33,16.36] az139.4 el0.3 and [4974.90,4002.87,17.71] az143.2 el-8.6. Windowless rooms receive excessive light. |
| OFP aerial forest | Remove grid/patchwork appearance while retaining original ground-level compatibility. |
| Fusion distance | Distant islands remain shadow-like, source-map boundaries visible, far forests absent. Satmap implementation alone has not closed this. |
| DLSS terrain | Vertical freefly movement smears ground textures. |
| Fusion editor | No editable locations; replace the 2049-sample/full-copy gate with bounded editing, rather than merely raising limits. |
| Fine brush | Support actual terrain detail below the 4.7m effective radius, not a smaller misleading slider. |
| Fusion coast | Vibrating water at [36392.88,27289.82,28.18] az235.6 el-39.9. |
| Showcase glass | Visible from both sides and breakable; earlier walk-block/clear-pane checks did not prove either. |
| Rocket craters | Owner now explicitly requests enabled by default. Verify first-hit/salvo cost and reset. |
| Smoke | Roof penetration remains even where walls contain it. |
| Foliage rays | OFP Everon [4320.38,4270.16,109.82] az306.4 el-0.1 at 17.62h; not all trees affected. |
| Water impacts | Rockets must produce larger splashes than rifle impacts. |
| Weather panel | Snow readily accessible, no collapsed experimental-only presentation; organise collapsible sections. Snowstorm currently appears ineffective. |
| Local lights | Intermittent village AND vehicle light loss after player spawn/freefly; variable still unknown. Do not close from one non-reproduction. |
| Box3D interiors | Everon bar [5092.16,3994.46,18.34] az195.6 el-14.9: shot props pass through walls. |
| Physics expansion | Enable appropriate original furniture/radio props, preserve mission interactions; investigate soldier ragdolls separately, with compatibility/performance gates. |
| Arma 1/2 | Investigate generation-specific base-config mounts instead of blindly mixing CWA/A1/A2; command semantics and scheduled execution need real tests. No leaked source. |

The developer quotation is input to investigate, not proof of implemented
compatibility. Existing ARMA12-DIRECT-MISSIONS plan remains authoritative until
updated against code. Returning dummy values for unknown SQF commands is not
accepted as compatibility; report unsupported commands explicitly.

At this backup checkpoint, the latest spray change is source/test verified but
NOT yet installed/runtime accepted: world-cell identity now drives jitter and
lifetime instead of camera-window instance index. A 0.9375m anchor crossing
previously re-randomised all candidates. Unit reference covers positive/negative
anchors and 17x17 window shifts; shared shader composition passes. The candidate
defaults on with WGR_SPRAY_WORLD_ANCHOR=0 as control; build/deploy/moving-camera
verification is next before treating it as accepted. No extra particles/bindings.

## NHO normal convention and tint regression tests, September 10

Native beach_naturedebris_01_nho.edds is present in the Forest material log.
NHO also stores normal XY in RG, with height in blue and occlusion in alpha;
its missing filename classification is now included for exact basename suffixes,
case-insensitively. Directory names and albedo suffixes do not match. Existing
NTC legacy control is unchanged. This fixes normals, not NHO height/AO shading.
267 channel/decode assertions pass. Installed d224e0ae+dirty at 08:21 passes both
compressed and decoded NHO visible-crate tests with identical RGB(51,51,69),
normal binding verified, no renderer errors, exit 0.

Broader [edds] initially exposed two stale RFG-070 tint tests. They expected the
retired target-mean rescaling while production defaults to RFG-071 linear
multiplication. No tint renderer change: the tests now run both explicit modes,
restore the previous atomic setting with RAII, and check independently calculated
RGB values and alpha preservation. Shuffled [edds] seed 10910 passes all 20
cases / 384 assertions. No claim that these pre-existing test failures caused
the owner's visual report, or that every texture family is now fully shaded.

## Direct RG normal parity, September 10

The earlier NTC correction covered retained draws only. Direct draws still read
normal X from alpha and treated red as parallax height. A per-draw RG bit now
reaches the existing material uniform through all three normal-binding routes;
RG textures skip the NOHQ red-height parallax path. Decoded DDS keeps the same
declaration because it retains red while copying normal X to alpha. No added
texture fetch, render pass, binding or FFI structure size. Ordinary AG/NOHQ maps
keep their channel convention. WGR_LEGACY_DIRECT_NORMALS=1 is the control.

Installed 7805be95+dirty, 08:04 build: Test-DirectRgNormals uses a generated 4x4
BC7 fixture and the installed original ammo crate. First images were rejected:
the initial camera missed the crate. Correcting the camera exposed a second bug,
a later d.flags=0 erasing the RG bit; that reset was removed before acceptance.
The durable NormalViz test checks the visible crate pixel, not merely a binding
log. Final compressed RG and decoded RG both give RGB(51,51,69); old compressed
AG interpretation gives (30,50,64). The separate AG fixture gives (51,51,69) and
must not bind the RG bit. All four runs exit 0 with no renderer errors.
81 gfx3d tests and six shared shader tests pass, including real-device pipeline
creation. Single-frame timings are not a performance claim. NTC cavity and
transmittance shading remain separate work, not implemented by a normal fix.
Native Forest production regression also passes after a 90-second warmup:
22360 registered / 6443 main instances, 8,058,216 main triangles, 22.016ms GPU
single capture, exit 0, no renderer errors. Image inspected; the remaining pale
branch-card appearance is not declared fixed. Evidence: build/direct-rg-normal
and build/native-reforger-perf/Forest-direct-rg. Cargo.lock hash unchanged.

## Parked DDS test resolved, September 10

The previous order-dependence diagnosis was not supported by reproduction.
The failing synthetic_dxt5 roundtrip carried [.fixture], excluding it from the
unfiltered suite, while an explicit [DDS] selection includes hidden cases.
It expected 64x64 from the 32x32 DXT5 source (the neighbouring DXT1 fixture is
64x64). The other DXT5 decoder/format tests already explicitly require 32x32.
No texture converter or source fixture was changed.

The regression now checks independent PAA metadata for 32x32, the 1024-byte
top-level block payload, exact dimensions and raw bytes of every DDS mip. Removed
the hidden tag because this test writes only an RAII temporary file, not a
repository fixture. Before: [DDS] 16/17 cases pass, 32==64 failure reproduced.
After: shuffled [DDS] seed 10910 passes 17 cases / 151 assertions; isolated
regression passes 31 assertions. The obsolete parked entry is removed rather
than leaving a completed defect presented as open.

## Bounded local-light shadow reach, September 10

Lighting uses a per-point endAttenScale (spots use 10), but local shadows used
only the unrelated rangeScale=6. A counted Everon lamp has start=9 and scale
55.555553: lighting reaches 500m while shadow projection/caster admission ended
at 54m. LocalShadowReach now uses at least the light multiplier, with the existing
180m cap retained. Projection, direct caster admission and cache key share the
same result. WGR_MATCH_LOCAL_SHADOW_REACH=0 retains the old comparison arm.
1210 native assertions pass, including legacy parity, malformed inputs and caps.

Same installed 5a303ce5+dirty binary: truck-on actual Zeus near/far/return A/B
retains all 42 candidates/selected lights and the live player; all exits 0,
no renderer errors. Inspected near captures show the shadow continuing onto the
hillside rather than stopping at the smaller projection. The lamp trace confirms
54 -> 180m, while capped truck lights remain 180m. This does not reproduce or
close the owner's intermittent missing-village-light report.

Measure-LocalShadowReach.ps1: two 60-second settled native captures per arm,
1280x720 MSAA4, same camera/profile. Cached arms have 52507 registered / 16806
main instances each. Local-shadow timer -1 is skipped/unavailable, not a zero-cost
claim; whole GPU frame control 9.830/8.953ms, matched 10.164/9.779ms.
Forced-refresh arms have 52507 registered / 16245 main each: local-shadow GPU
control .76/.74ms, matched .76/.78ms; CPU .09/.08 versus .09/.10ms. Whole frames
11.027/11.164 versus 11.305/11.741ms. Do not compare the changed main-instance set
across cache modes or claim a speedup. Bounded reach matching is accepted as the
default correction. Final default build and installed truck-on Zeus smoke pass
(74625b20+dirty): no opt-in environment value, trace confirms 180m, all 42 lights
remain selected, live player preserved, far/return images inspected, exit 0.
Full ShadowMath filter passes 48 cases / 1891 assertions. Beyond 180m and shadow
light-budget selection remain limitations, not fixed by increasing this multiplier.

Evidence: build/freefly-lights/acceptance/reach-{control,matched}-truck-on-* and
build/local-shadow-reach/{control,matched,control-refresh,matched-refresh}.

## Snow settings reset gap, September 10

The master reset omitted all six snow settings. ResetSettings now copies their
declared defaults without clearing accumulated depth, precipitation history or
stored track chunks. Master reset and a separate Weather-tab button use it;
Clear snow and tracks remains the explicit destructive action. Snow is disabled
after the settings reset and becomes visible again when re-enabled.
Native exact snapshot preservation test passes (15 assertions); the complete
[Snow] filter passes 10 cases / 9919 assertions. Build and installed
Test-SnowCrawl -ResetSettings pass on b771c8e8+dirty with these changes. The
harness-only dev_snow command exercises the same reset entry points and reports
settings/depth/chunks; it does not add a savegame format or alter mission scripting
commands. Stored depth 0.18m and eight chunks survive disabling/re-enabling.
Inspected trail, disabled and restored captures confirm the persistent groove.
Restored falling=true also reinstates overcast, so the restored picture is much
darker: these images are track-preservation evidence, not equal-lighting material
acceptance. Evidence: build/snow-crawl/acceptance.

Sky extras and road conform previously called Rust directly from producer-side
setters. They now travel in the existing frame-owned settings queue and are
consumed from that copy. SetSkySettings also includes the separate cirrus, fog
close and godray parameters, fixing the master reset gap and removing duplicate
Sky-tab calls. No FFI layout change. The installed harness deliberately changes
the settings before master reset; consumer-side WGR_SETTINGS_QUEUE_TRACE proves
cirrus/rays false -> true, intensity 1 -> 4, fog close .4 -> .85, softness .9 ->
.45 and road lift .03 -> .02 with thread=1 overlap=1. Exit 0, no renderer errors.
This removes specific unsafe calls; it does not prove all renderer ownership is
correct or attribute the owner's earlier Windows freeze to these calls.

## Rifle-impact shadow regression controls, September 10

Test-ImpactShadows.ps1 makes the earlier Desert fixture durable and adds Stratis
using the installed original-world override. Same 0b684058 runtime: repeated
BulletSingleW impacts on dry terrain, legacy smoke shadows enabled at strength
2.50. Last three noon samples: Stratis 57/54/45 cloudlets and Desert 51/42/51;
all have zero legacy and total shadow blobs. Both exit 0, renderer error checks
pass, noon captures inspected. Earlier 06:00 controls also count zero blobs, but
Stratis is too dark for useful visual acceptance. Small dark dust sprites and
the authored impact decal still exist; this is not a claim that every dark
impact-shaped mark reported by the owner was a shadow or has been removed.

The first Stratis fixture was rejected correctly: unknown .Stratis mission
suffix resolves to Intro in the installed configuration, so its init.sqs was
not found through the legacy mission directory. Retaining .Intro for the test
mission and using --test-world for the actual Stratis terrain fixes the fixture;
it is not a new engine fix. Accepted captures: build/impact-shadows/Intro-hour12
and Stratis-hour12. No global disabling of smoke, shadows, wind or dust.

## Resolved breaker-foam experiment, September 10

WGR_BREAKER_CELLS=1 tests opaque near-field foam patches rather than uniformly
translucent wave foam over glossy water. Uses the existing filtered structure,
no extra noise/hash/texture sample or simulation pass. Only wave-generated foam
is reshaped; zero wave gain remains zero and distant/subpixel coverage returns
to the previous filtered result. Default OFF until same-binary native/DLSS and
calm/zero-gain comparisons establish the effect and cost. Tests/build/runtime
acceptance pending; not a finished quality improvement.

6c70c6a5 first installed candidate: 21 water tests and build pass, native exit 0.
Near patches become white but look too uniformly painted; not accepted as the
default. Follow-up adds one band-limited value-noise octave only inside resolved
near wave-foam patches, and lets their rough diffuse normal follow more of the
wave slope. This changes the earlier zero-extra-noise claim: still no texture
fetch/pass/FFT increase, but bounded extra ALU needs measurement. Default OFF.

b771c8e8 refinement: 21 water tests/build pass. Same-binary native candidate
draw samples 3.01-3.41ms versus control 2.99-3.40ms, one pair only; no headline
performance claim. Native, AUTO/DLSS, calm and zero-wave-gain launches exit 0
without renderer errors. Calm/zero-gain captures inspected; the bright diffuse
water reflection must not be mistaken for foam. Near white patches are still too
flat/paint-like for default acceptance. Keep the refinement opt-in and retain
the already accepted foam-history and dynamics-filter corrections independently.

Further bounded experiment: WGR_BREAKER_CELLS=2 adds world-hashed variable-size
microbubbles only to resolved wave foam; 1 retains the previous control, unset
remains 0. No texture/pass/FFT increase, nine nearby-cell hashes when resolved,
early average return before hashing at subpixel distance. The first 16cm-cell
version looked polka-dotted and was rejected. The refined 5.8cm-cell version with
variable bubble radii removes the large dots but still looks too surface-painted
for a default quality claim. It remains opt-in, not a completed whitewater system.

Installed 6f10fc95+dirty 08:39: native fine-bubble draw samples 3.018-3.445ms;
same-binary previous-cell control 3.006-3.436ms. Overlapping single-run ranges
are not proof of zero cost. DLSS Quality 858x482 -> 1280x720 also runs normally
(draw 2.145-2.374ms, not directly comparable to native). Native/DLSS, zero-gain
and calm captures inspected; all exit 0 without renderer errors. Zero gain and
calm remove whitecap patches; bright water reflection remains, correctly.
Evidence: build/breaker-foam/acceptance/*bubbles*-fine.*; old coarse pictures are
kept under the same names without -fine. Synthetic coverage guards and entry
shader composition pass. The quality gap now needs better breaking-sheet/raft
structure, not progressively finer white noise or a global foam brightness boost.

## Late surface-rays experiment rejected, September 10

0b684058 tests per-instance fragment-distance scattering on prepassed opaque
world draws after water, without changing replay order. It reuses the existing
alpha-surface integration and bindings; glass/cockpit/monitor/decals are excluded.
The slot-selection test and build pass. WGR_LATE_SURFACE_RAYS remains OFF by
default, independently of the earlier rejected WGR_EARLY_LATE_OPAQUE experiment.

Installed same-binary native Stratis Zeus comparisons exit 0 and exercise
479-493 marked slots at intensity 4. Both surface and surface-repeat captures
show an unacceptable change to the hillside haze, not an accepted foliage fix.
Control retains the lighter hillside; the candidate colour-pass samples are
around 11.0ms versus 9.5-10.3ms control. This is not a measured speedup, nor
proof that a globally darker image fixes black tree silhouettes. Shadow helpers
both return occlusion (zero lit); their convention is not inverted here.
Root cause of the wider image difference remains open. Do not enable by default.
Evidence: build/foliage-rays/acceptance/{surface,surface-repeat,surface-control}-
0b684058-rays1.{png,log}. Test-FoliageRays now defaults to the unchanged path;
the old rejected order requires explicit -EarlyOpaque. Renderer errors and an
unexercised candidate reject the test instead of accepting a screenshot alone.

## Cold indoor-smoke build work, September 10

4985db47 bounds the same seven shell samples to each component's conservative
cell box. Fire/View union, .25m resolution, room flood and portals are unchanged.
37 Smoke/BuildingInterior/Wind/Airflow cases pass 28,611 assertions, including
full-cell reference parity for thin/slanted/overlapping/outside components.
Installed same-binary POSEIDON_INTERIOR_COMPONENT_RASTER=0 control: hasic_zbroj
raster 6.611 -> 2.201 ms, whole build 31.1 -> 24.8 ms. Exact shell hashes match
for all four reached building types (fire station: 8460896540168774620). Both
30-second runs exit 0, around 3900 smoke particles and 3300 labelled indoors;
late simulation stays around 2ms. No particle, wind, downwash or containment cut.
First pre-change fixture's final check mistakenly omitted the ResHouse name
prefix; log confirms a valid 31.0ms build, not a failed load.

The remaining cold hitch is not declared solved. Follow-up indexes the four
coplanar face neighbours rather than scanning every face for every BFS entry.
Sorted neighbour indices preserve exact original visit and float summation order;
synthetic reordered multi-plane tests pass. Final room/portal checksum and installed
same-binary acceptance remain pending for that follow-up.

8c9c4a87 acceptance: 38 targeted cases / 28,614 assertions pass, build passes,
clean installed same-binary indexed versus full legacy control both exit 0.
Fire-station total build 4.3 versus 31.5 ms. Final labelled-grid AND ordered
portal scalar checksum matches for all four reached types; fire station
11083875832578533832. Room count 1, six portals and 1581 uncovered leak faces
remain unchanged. This is faster generation of the same existing containment,
not proof that its pre-existing inferred breaches are physically perfect.
Exterior capture inspected without a new escape plume. Late smoke still roughly
3900 particles / 3300 indoor; no settled-frame FPS or all-building hitch claim.
The earlier component-only step's remaining 25ms hitch is superseded by this
indexed-face result, while larger unseen building types still need coverage.

## Water normal-filter completion, September 10

2601365f fixes the remaining wrong bicubic weights in texture_bicubic_dynamics.
fa7a7af2 had only corrected displacement; the old whole-file substring test passed
because that other function contained the correct formula. The tightened test
first fails on the unchanged dynamics function, then all 20 water tests pass.
Both axes now combine adjacent taps with their own denominators and the correct
high-pair interpolation weight. Four texture fetches remain four; no new pass,
texture, FFT resolution, global blur or reduction in wave geometry.

Installed 89809008 before versus 2601365f after: Nogova offshore at 13000/3000/8,
az 90/el -30, wind 18, wave amplitude 2.2, exposure .25, native 1280x720/MSAA4,
shore foam 0 and wave foam 0. The strong rectangular specular texel lattice is
visible before and absent from the near-water after capture. Raw before files:
build/breaker-foam/normal-filter-before-89809008; after: acceptance/*gain0*native.
Water draw samples remain approximately 3.0-3.2 ms in both runs. Wave phase varies
slightly, so this is visual artifact acceptance, not pixel-exact image parity or
a headline FPS comparison. Both runs exit 0 without renderer validation errors.

Additional wave-foam .62 captures exit 0 in native and DLSS Quality (log confirms
858x482 -> 1280x720). Foam and highlights remain visible without the near-field
rectangular lattice. These do not certify all distant mesh facets or moving-DLSS
ghosting, which remain separate issues.

## Glass station and counted palette acceptance, September 10

89809008 adds a genuine tinted, transmissive GLASS station separate from GLOSS.
Generated original P3DM/PAA assets include a thin collision pane, frame and NORMAL
reference behind it; 4 generator tests pass, including partial alpha through all
eight mip levels. Installed Test-Showcase -GlassOnly exits 0. Front/oblique/removed
captures in build/showcase/glass show transmission and surface reflection/tint.
The directed walk stops at z=3503.62 before the pane at 3504; after removal the same
approach crosses 3504. The pane is deliberately not shatterable.

Fixture correction: live input restores the mission's 30-degree heading after
setDir 0. The first long walk went around the pane, not through it. The final short
approach checks the actual heading; fixed scripted cameras handle visual A/Bs.

dcf2ae88 opt-in upload trace, installed in 89809008, confirms 901 palette slots,
8192-byte stride and 3 cameras for 320 soldiers. Batching is actually used: one
upload replaces 901 calls with byte-identical data (two Rust parity/fallback tests
pass). Same-binary legacy control WGR_PALETTE_UPLOAD_BATCH=0 is exercised twice.
Worker frame setup: batched 1.595 ms, legacy 2.844/2.946 ms. CPU frame mean:
16.409 vs 17.771/18.000 ms, p95 17.992 vs 18.140/18.288 ms. Prior batched b71ac83f
setup samples about 1.60/1.57 ms agree. Both arm captures inspected; 321 unit thinks
remain active, no renderer validation error. Keep this bounded upload reduction;
do not describe it as a general AI scalability fix. GPU timestamp span varies
15.052 vs 22.193/16.986 ms, and queue submit remains about 12 ms. Combat, DLSS,
vanilla comparison and longer frame-time sampling still need separate acceptance.

## Large-unit rendering investigation, September 10

Measure-AIScale.ps1 generates 80/160/320 idle soldiers plus a live player in
8-man groups on stock Desert, seed 1234, 1280x720 native/MSAA4, 60 s warmup,
real-time simulation. It checks the final active think count and applied camera.
The Away arm turns 180 degrees in place, not a player teleport or simulation LOD
change. No AI behavior was changed for this measurement. Combat, vanilla and
larger populated worlds remain separate outstanding comparisons.

Installed 883f174a first near samples: CPU frame mean 9.689 ms (80) and 17.711 ms
(320), with AI about 0.11 and 0.42-0.49 ms/tick respectively. The 320-away arm is
9.305 ms/frame with comparable AI cost. `land:obj` + `drw:land` falls from about
4.45 to 0.074 ms/frame on turning away. The sparse scene has zero RETAINED instances;
that metric does not count these direct animated soldier draws. Captures inspected.

The 320 near repeats are NOT stable enough for a headline speedup: timestamp frame
span is 16.34 / 22.55 / 23.41 ms while attributed GPU regions stay around 9.7-9.9 ms.
Worker queue submit is a major CPU cost (12 ms in the first sample). Serial-renderer
and compute-skin controls improve one sample but do not justify changing defaults;
the serial arm has worse p95. Do not sum CPU and GPU time or call the timestamp
residual shader cost. Render/governor and upload costs need isolated controls.

Concrete no-detail-loss candidate d97b1b99: the already contiguous 128-matrix
palette blocks were submitted individually. Batch only when dynamic UBO stride
equals packed block size; preserve per-slot fallback for padding and the
WGR_PALETTE_UPLOAD_BATCH=0 control. Byte-parity tests include multiple palettes,
NaN payloads and trailing incomplete data. Installed A/B/build acceptance pending.

Rejected fixture arms: `current-883f174a` omitted the camera (helper warned);
`pose-883f174a` tried absolute stock Intro as a world replacement, failed to load
that terrain and hit the bounded process watchdog. Neither is a valid performance
baseline. Camera-only CLI positioning on the normal mission path works and is the
current harness route. No user process was stopped and no Windows crash inferred.

## Native NTC comparison, September 10

Installed b4d118ce Forest same-binary A/B completed: fixed 22.726 ms GPU,
legacy 24.244 ms, both 22360 registered / 6452 main instances. Single samples
with slightly different triangle counts are not evidence of a speedup. Captures
in build/native-reforger-perf/Forest-ntc-{fixed,legacy}-b4d118ce show stronger
directional relief in pine crowns with the corrected RG normal input; flat
branch cards remain. No additional texture samples or rendering passes added.
The deployment's +dirty marker was an AI-assessment documentation edit only.

Initial synthetic NTC/NMO decode assertions passed; its BCR control incorrectly
expected coverage alpha 200 instead of this importer's intentional opaque 255.
Corrected test also covers ordinary coverage and basename-only NTC recognition.
The broader EDDS suite additionally reports two pre-existing layer-tint
expectations incompatible with the existing linear tint path; no all-suite-pass
claim. Final corrected targeted test/build acceptance follows separately.

3f736b26 final acceptance: native [ntc] test passes 210 assertions, build passes,
clean-stamped deployment and Forest installed capture exit 0. Final capture
23.242 ms GPU / 22360 registered / 6451 main instances, visually consistent with
the earlier fixed arm. No claim that all remaining native material issues close.

Roadmap safety follow-up a23c43de adds an opt-in archive maximum-version check
at the first line of World::Serialize, before campaign/global/entity mutation.
Existing old-version per-field defaults and other archive types are untouched.
Native tests cover old/current/future versions on both passes; build and installed
current-version round-trip acceptance pending. This does not solve the separately
parked static-object identity or network-authority problems.

883f174a acceptance: build passes; [save],[ntc] passes 14 cases / 327 assertions.
First new fixture reset a primitive between loading passes, contrary to the
existing first-pass-only primitive reader; corrected without changing production
deserialization. Installed Test-SaveVersion current-883f174a exits 0: save returns
OK, load returns OK, player alive and damage restores 0.5 -> 0.25. Future-version
rejection is a native test, not an installed corrupt-save UI acceptance claim.

## Renewed owner scope after the follow-up audit

All remaining measures proposed in REQUEST-AUDIT-FOLLOWUP-20260909.md are now
authorised work, not completed work. Execute small fixes before architectural
changes; preserve measured baselines and installed-game acceptance. New requests:

- Editor first: wheel radius, Left Alt temporary opposite operation, requested
  radii below 8 m with truthful grid limits, reset all terrain craters, safe
  Fusion editing. True sideways roofed caves remain a separate 3D collision task.
- Snow: reduced road accumulation, crawling displaces snow, fix distant road
  visibility changing through snow with approach. Preserve shelter protection.
- Breaking-wave/whitecap foam, NOT more shoreline foam; measure quality/cost.
- Fusion: high-altitude blue/untextured island sections, far albedo and actual
  inter-island visibility. Do not hide defects with fog or lower geometry detail.
- Freefly night lighting: repeat with live player and truck headlights before
  entering freefly, not only static village lamps.
- Arma 3 at player [1747.27,5038.25,4.49], camera [1747.43,5037.95,5.85], az81.9
  el1.2, 06:00: distant foliage too dark under rays and ground-impact shadows.
  Exact world still to be identified from installed worlds/fixtures.
- Native Reforger: contrast/normal/material/forest quality versus converted
  reference, plus performance without concealing missing content.
- OFP forests from the air: tile-like distribution and appearance; compare
  candidate impostors/variation from ground AND air before adopting.
- Review PARKED.md and POST-DLSS-MODERNISATION-ROADMAP.md after the small items,
  revise priorities against actual shipped work and retain rejected experiments.
- Plan direct Arma 1/2 mission loading from original game directories, without
  copying assets into our engine directory. Inspect inspiration/officialarma;
  owner can install missing games tomorrow. Do not block unrelated work on this.

First editor input change is in progress: wheel consumes input only with the
hidden-panel active Zeus brush, Left Alt reverses each held stroke and preview,
requested slider minimum 0.5 m. Effective 1.5-cell support is deliberately not
misrepresented as sub-grid geometry. Build and installed acceptance pending.

0656f596 built/pushed/deployed. Two radius cases / 29 assertions pass. Installed
hold/release/raise test exits 0: 29.835 -> 24.0394 -> 16.8815, released unchanged,
raised 23.01. editor-controls-radius.png inspected: ring and input banner visible.
This harness does not yet exercise physical wheel/Alt input; do not call those
two controls visually accepted on this evidence alone.

Terrain restore in progress: bounded pre-first-edit snapshot (up to ~16 MiB at
the existing 2049 editing cap), shared by brush/rocket/LAB paths. Explicit
confirmed action replaces physics terrain transactionally before visible heights;
settings reset remains non-destructive. Baseline cleared on mission shutdown,
grid changes refuse mismatched restoration. This does not yet enable Fusion editing.

Installed aeaab9b2 acceptance: terrain-restore-fixed test uses triTerrainPaint
to decouple height/collision acceptance from desktop mouse movement. Crater
29.835 -> 23.835 -> 29.835; hill 23.535 -> 27.535 -> 23.535; repeated edit/reset
also returns 29.835. Exit 0, restored image inspected. First hold-based run was
invalid: screenshots show the physical cursor moved to the upper right while
the harness kept sampling its old centre. No engine failure claimed from that run.
Lifecycle follow-up 00b82a41 clears the backup on World::InitLandscape and cleanup,
not merely process shutdown. UI confirmation clicking and fresh-world reset
acceptance still need separate coverage.

aeaab9b2 adds prone-body snow pressure at <=10 Hz, independent of footstep audio.
Only grounded living prone soldiers affect ground snow; elevated roadway and sea
checks retained. Capsule-like 0.6m-wide pressure uses the existing persistent
SnowField, no gameplay RNG or movement changes. 12 snow/terrain cases / 9970
assertions pass. Installed scripts/Test-SnowCrawl.ps1 exits 0: soldier crawls
to [9700.16,3605.15,-0.00137329]. crawling.png and trail.png inspected: broad
depression remains after moving the actor away. Existing angular snow mesh edges,
physical snow contact and salted-road issues are not resolved by this change.

Breaking-wave foam investigation: persistent red-channel coverage is smoothstepped
every frame after physical decay/injection. It destroys thin rafts even at zero dt.
Pending fix retains coverage linearly and makes breaker diffusion dt-based; same
textures/passes, unchanged shoreline and wake-channel formula. Runtime A/B switch
WGR_BREAKER_HISTORY_LEGACY=1. Numerical/shader tests and installed images pending.

be66665d installed: three history/numerical/shader tests pass. Bright matched
offshore captures at wind 18, shore gain 0, wave gain 0.62 show the gain works
(gain 0 removes the matte rafts; gain 2 strengthens them), but the desired
breaking-crest quality is NOT accepted yet. Foam-pass median about 0.17 ms in
both history modes; no claim of overall FPS improvement. Initial test profile
used the wrong world filename; corrected to noe_noe_wrp.cfg and asserted the
applied amplitude. Exposure 0.15 was too dark; fixture now uses 1.0.

Next bounded experiment: deep-water suppression still attenuates even strongly
compressed crests. Recover direct crest readiness only as the existing Jacobian
breaking signal rises; retain calm-wind gating and unchanged shoreline readiness.
One numerical regression test passes. WGR_DEEP_BREAKER_RECOVERY=0 preserves the
previous direct-crest path for same-binary A/B. Installed validation pending.

a2f9a519 installed A/B completes: direct crest recovery visibly strengthens bright
breaking caps at wind 18 without increasing shore gain; calm wind 0 remains free
of whitecap patches. Median foam pass ~0.171 ms, surface draw ~2.33 ms versus
~2.33 ms with legacy crest readiness in these short captures. This is not an
overturning fluid simulation and the blocky reflection remains a separate defect.

Fusion automatic far albedo in progress: eligibility now budgeted through 2048
land cells and 4094 source textures; composite uses one 2048-square mipmapped image
(~21.3 MiB). Single-island <=512-cell output remains 1024. Existing Fusion log
has 1093/4096 bound layers and zero dropped layers, so the 1024 page budget is
NOT evidence for missing classic textures (that budget is modern paged-path only).
High-altitude capture added to Test-Fusion; blue island report not yet diagnosed.

0b176556 installed Fusion acceptance exits 0; 2 satmap cases / 38 assertions pass.
2048-square bake from 1092 tiles takes 727.3 ms once, 1094/4096 bound layers and
zero dropped. Overview and 40km-high captures inspected: all five islands retain
albedo. Per-island visits pass. Working set about 2.3 GiB with AND without bake;
do not attribute the difference from older 1.8 GiB runs to the new 21 MiB image.
Forest distribution still forms original patches; satmap is not new vegetation.

eefa6161 adds triFreeFlyPose through the actual Zeus enter/coordinate path and
Test-FreeflyLights. First fixture incorrectly used 5tTruck instead of Truck5t,
fell back to menu and was rejected by image inspection. Fixture now verifies
vehicle type and startup error latch. String results require trimming evaluator
quotes. AI SAFE drivers override LIGHT OFF every tick; control uses COMBAT and
checks 40 selected lights versus 42 with SAFE/headlights. Both valid runs exit 0,
living player retained, near/far/3km/return captures inspected. Distant pools remain
similar in both arms; no truck-triggered lighting disappearance proven yet.

Direct A1/A2 mission plan: ARMA12-DIRECT-MISSIONS-20260910.md. Original archives
inspected in place: A1 Armory and A2 Bootcamp/campaign content exists. External
archive mounting already implemented; scheduled SQF, archive-backed nested mission
selection, conversations/FSMs and mission completion are separate compatibility
gates. No source assets copied and no original mission claimed playable.

Stratis dawn follow-up (75aceb93): WGR_LATE_OBJECT_TRACE confirms late opaque and
cutout colour writes after volumetrics (9 solid / 19 cutout in the Zeus fixture).
Test-FoliageRays uses the reported pose, June 21 06:00, actual Stratis WRP override,
fixed manual exposure, native MSAA4; -Zeus retains a living player away from camera.
Initial missing-world run loaded Intro and was rejected. The WRP override log still
names Intro in SwitchLandscape; validate the actual terrain-material load instead.
dlssMode=-1 means automatic, NOT disabled; this fixture now explicitly writes 0.
An initial pre-water replay produced bright triangles without rays. After-water
replay removes those (hillside no-rays mean absolute RGB delta 0.142/255), but both
after-water and after-cloud replay unexpectedly reduce broad hillside scattering
with rays. Same result with real Zeus rather than the CLI camera, which also shows
player foreground/occasional underwater-state transitions. Therefore the ordering
experiment is NOT accepted: WGR_EARLY_LATE_OPAQUE is default OFF at 75aceb93.
Images/logs: build/foliage-rays/acceptance, zeus-legacy/zeus-after-water/zeus-shafts.
One classifier test passes; that is not visual acceptance or a fix claim.

PARKED and POST-DLSS roadmaps now have dated current-authorisation overlays:
native load and render overlap no longer incorrectly treated as unimplemented;
historical capability census retained as historical, GL33 retirement deferred.
Full remaining-item recensus is not claimed complete.

Next small lighting correction: partial cube-shadow visibility was ignored by
back-facing ambient (fully shadowed points already skipped). Reuse point_occl
instead of 1, preserving unshadowed and spotlight behaviour, no additional samples.
Shader/numerical guards and installed acceptance pending. This is separate from
the still-unreproduced whole-town light disappearance.

12f6ada6 installed acceptance: the point-shadow ambient change passes four Rust
shader tests. Test-FreeflyLights exits 0 with a living player, truck headlights,
42 selected lights and near/far/3km/return captures. The distant image retains
light pools. This is an integration smoke test, not a controlled partial-penumbra
visual comparison or resolution of the owner's intermittent town disappearance.

Fine brush support is now 0.75 grid cells instead of 1.5 (4.6875 m at 6.25 m
spacing), not invented sub-grid geometry. Three cases / 634 assertions include
off-vertex positions. Installed Test-TerrainFineBrush: centre 29.835 -> 26.835,
next vertex outside radius unchanged at 29.835; cell-centred stroke still affects
the grid, and reset returns 29.835. Capture with exposure 0.15 inspected; shape
contrast is subtle, so numerical checks establish radius/reset, not rounded detail.
The first exposure-1 capture was washed out and is not visual acceptance.

AI architecture assessment completed without implementation:
AI-ARCHITECTURE-ASSESSMENT-20260910.md recommends incremental augmentation of
the existing mission/task/FSM hierarchy, optional bounded group planning over
existing commands, and matched many-unit CPU/GPU measurements before threading.
It explicitly preserves baseline scripting, saves and authority semantics.

Native Reforger current 12f6ada6 captures: Village 22.731 ms GPU, 22,360 registered
instances, 7.907M main triangles; new fixed Forest pose (from PARKED) 23.381 ms,
22,360 instances, 8.470M main triangles. Both complete/settled, DLSS Quality/MSAA4.
Images inspected, flat native canopy reproduced. These are one-run baselines,
not an optimization or a fixed-geometry comparison with historical runs.

Snow-road reproduction at the reported Everon village: 0.3m accumulated snow
hides the near road but the far view exposes it. Same binary with tiny road depth
lift hides it at both ranges. Pending fix lends the existing terrain snow buffer
to OnSurface fragment shading and rejects fully buried, ground-pinned fragments;
no new texture/upload/pass. The dry path and rigid bridges retain their depth
behaviour; roof/track deficits use identical depth evaluation, guarded by a parity
test. This fixes artificial exposure, not salted-road accumulation. Installed
snow/dry verification remains pending before acceptance.

43fce26b first installed run REJECTED: shared conform layout exposed nine fragment
storage bindings to GPU-driven pipelines against the device limit of eight.
Game shut down; this was a pipeline validation failure, not a Windows freeze.
Correction keeps the original two-binding conform layout for retained/velocity/
GPU-shadow pipelines and lends snow only to direct-draw pipelines. Device limits
are not raised. Fixture now rejects validation errors before continuing captures.

1ed79e6f corrected installed snow/dry runs both exit 0, living player checked
at every pose and no validation errors. Far road remains buried at 0.3m snow;
dry far road is visible. Images inspected. An intermediate run moved the player
to an unverified coastal position and ended early; removing that fixture change
restores completion. Do not infer a renderer crash from that disconnected harness.
A real-device layout-budget test also passes, using the actual conform layouts
alongside a saturated fragment-storage budget, complementing six shader tests.

Native forest next finding: live Forest log binds picea/betula/corylus NTC normal
maps, but IsEnfusionRgNormalName only recognised NMO. NTC alpha is cavity, not
normal X (Bohemia's official Textures channel table). Pending correction recognises
NTC in both compressed flags and decoded R-to-A repacking. This is an input decode
fix, not a contrast/SSS tuning change; no new GPU sample or geometry. Same-binary
control POSEIDON_LEGACY_NTC_NORMALS=1. Synthetic channel tests and installed Forest
comparison pending. Source: https://community.bistudio.com/wiki/Arma_Reforger%3ATextures

Owner authorised implementation after backup `codex/backup-asset-obs-20260909-bc766e19`.
Worktree asset-obs only; foreign Cargo.lock excluded. Screenshots 070351, 070456,
072225 and 073829 are reproduction evidence, not completion evidence.

## Queue and acceptance

- Experimental destructive terrain: real height/collision changes with bounded
  invalidation, not a decal or snow-only depression. Do not claim tunnels yet.
- Showcase: verify normal/gloss examples (owner wrote glass), add actual glass
  if appropriate; large shootable block castle/walls; enterable dark rotating-light room.
- Snow: visible flakes, roof exclusion for accumulation, ground variation and
  actual geometric tracks/depressions, not solely albedo/normal shading.
- OFP satellite far appearance: prototype now explicitly authorised for quality.
- Ocean: camera-independent continuous coverage and no brown offshore tiles.
- Water specular: investigate blocky near reflection with bounded extra cost.
- Wave foam intensity: independent gain and profile persistence. In progress.
- Lighting reset: dedicated button plus master reset; advertise actual coverage.
- Lamp and vehicle bulb glow: replace visibly dated sprites, retain occlusion.
- Local shadow source size/penumbra and abrupt loss while light still reaches.
- Everon dirt-road glassy sheen at player 5083.78/3988.61/16.33,
  camera 5083.67/3988.25/17.63, heading 145.4, elevation -18.5.
- Distant freefly lighting with live player: reproduce two-camera regression.
- Picture Mode: max blur 4, optional player follow and click focus. In progress.
- Smoke: dispersal/dilution/despawn and ground shadow regression.
- Godrays on distant foliage/forest LODs: image 073829 contrast mismatch.

## Initial investigation

Installed baseline a4d09dbe. Private Nogova aerial at 30 km altitude shows water
throughout, not the owner's tile failure; 120 km exceeds useful camera/atmosphere
coverage and produces a different black-background failure. Neither is acceptance
of the reported view. Raw `.tmp-bulb/sep9-water-*-before` files retained.

Found: whitecap shader gain is multiplied by shoreline gain despite UI promising
independent controls, and whitecap fields are absent from water-look persistence.
Master reset already includes sky/water/roads but does not list them; lamp/glow/
shadow settings are actually missing. Fixes and tests in progress, not yet deployed.

## Implemented checkpoints (not overall completion)

- 684b2871: independent wave-foam gain, persisted whitecap settings; dedicated
  lighting reset and master reset coverage; Picture Mode max blur 4, optional
  player-follow and free-fly click focus. 58 C++ cases / 4759 assertions and
  whitecap Rust contract passed. Installed DOF lifecycle capture exits normally;
  interactive focus and foam A/B acceptance still pending.
- ac5eaaa9 / bcaec198: snowflake billboard length no longer collapses when viewed
  along its velocity. Optional close-up snow geometry refines selected terrain
  patches to <=0.125m samples within an 8m camera window, plus accumulation drift
  variation. Collision remains bare ground; accumulation roof mask still open.
- 4d14628d / 1714d132: large castle preset (485 dynamic blocks after adding a
  supported lintel), ~32m span, 8m walls, 11m corner columns; moving room light,
  material-room action entries. Build and 66 cases / 16007 assertions passed
  before lintel follow-up; follow-up has 492 assertions / 2 cases passed.
  Installed and pushed both safety refs; original backup ref untouched.
  `scripts/Test-Showcase.ps1` exits normally with castle and light-on/off captures.
  Images confirm standing gate and daylight room illumination toggling. Room
  still has visible edge gaps; normal/gloss contrast and actual castle impact
  remain acceptance work, not closed merely because the harness returned OK.
- Investigation found incorrect bicubic FFT dynamics reconstruction: non-adjacent
  pair sums, cross-axis denominator and reversed blend weights. Corrected using
  the same four texture fetches; numerical reference test added. Not deployed yet.

- fa7a7af2: corrected bicubic filter built, numerical reference passed, installed
  low-altitude ocean capture exits normally. Grid-like marks remain in that
  particular view, so this correctness fix is not full acceptance of the owner's
  near-water screenshot. Capture pair `.tmp-bulb/sep9-water-close-before/after.png`.
- c625e1ea: reversible experimental Showcase crater at 9700/3600, 24m radius,
  8m depth on Desert's 6.25m terrain grid (45 changed samples). CPU SurfaceY,
  GPU heightmap/quadtree and Box3D heightfield all update; water bounds refresh
  without restarting FFT. GPU material/grass atlases are not reuploaded.
  Replacement physics terrain is constructed before destroying the old one;
  dynamic bodies wake after replacement. Synthetic resting-body test confirms
  settling into the excavated bowl with the original body handle intact.
  62 cases / 15989 assertions passed. Installed harness exits 0, rejects a
  duplicate crater and restores it. Log centre 29.835 -> 21.835 -> 29.835m;
  `terrain-before/crater/restored.png` images show the geometry change.
  Limitations: explicit LAB actions only, full heightfield rebuild pause,
  single-player unsaved prototype, no automatic grenade damage, no tunnels,
  not a general solution for navigation or objects built over excavated ground.

- 8a4161a8 / 9bded011: automatic legacy far albedo built from decoded source
  tiles, 1024-square with mipmaps, wide cell-edge blending, transition 800-2400m.
  No baked light/snow, existing authored satellite/Enfusion materials unchanged.
  `WGR_OFP_SATMAP=0` disables it for A/B. Installed Everon: 241 source tiles,
  111ms startup bake; Nogova: 302 tiles, 485ms, ~5.3MiB texture including mips.
  Initial 256-tile cap excluded Nogova and was corrected to a bounded 1024;
  missing/undecodable source tiles abort the bake rather than inserting white.
  Installed capture exits normally and is smoother at range, but cannot invent
  photographed terrain detail absent from OFP source art. 11 cases / 13529
  assertions passed for satmap/crater/CDLOD before the follow-up.
- 9bded011: Everon dirt-road sheen reproduced at owner's exact camera pose.
  `WGR_GLASS_SHEEN=0` ablation removes it; geometry-aware glass classification
  now excludes OnSurface decals while retaining actual glazing's alpha rule.
  Installed default-on-glass capture confirms the road no longer has blue sheen.
  99 cases / 1140 assertions passed for Material/satmap/crater/LightVisibility.
- Room investigation: explicit vectorUp reads [0,0,1] for both roof and back wall;
  common ASL elevations are correct. Uprighting makes no difference. Bright
  edge bands therefore are not explained by slope alignment; investigate
  shadow leakage/rendered geometry before treating the room as finished.
- 8d2fc2f1: ocean axial-far vs radial-corner correction built, 18 cases / 13557
  assertions passed, pushed both safety refs and installed. Same 2048x857 camera
  at 80000/7000/40000, heading 0, pitch -80 shows increased coverage but still
  a stair-step ocean edge. This is partial, not acceptance. CPU water still clips
  at the land far plane despite the GPU using infinite-far projection. Follow-up
  removes that water-only clip and derives reach from sea/frustum intersections,
  bounded by the existing last ocean LOD range.
- afee9c8f: follow-up built, 10 CDLOD cases / 13494 assertions passed, pushed
  both safety refs and installed. Same camera now has ocean over the entire
  image, no stair-step edge. Draw timings in this view increase from roughly
  2.8-3.9ms to 4.1-4.9ms because formerly missing pixels are now shaded; FFT
  allocations/resolution unchanged. Not an infinite-orbital-space solution.
- Smoke follow-up in progress: surface shadow sampling incorrectly returned
  fully lit when cloud strength was zero, ignoring the independent smoke channel.
  Removed that gate. Growing-puff opacity is shared by sprites, self-shadow,
  ground shadows and volume injection; previously injection omitted expansion
  dilution and was multiplied by the ground-shadow control. Wall clipping no
  longer increases density. Initial 11 smoke cases / 409 assertions passed;
  final build, shader contract and installed comparison pending. No wind,
  downwash, collision or emission-lifetime changes in this patch.

Other queue entries remain open. In particular local lights currently contribute
only diffuse/ambient in `lighting.wgsl`; the glossy material's direct specular
term is sun-only in `shading.wgsl`. Night GLOSS demo is not considered fixed.

- c59deee1 smoke follow-up built, pushed both safety refs and installed. Rust
  cloud/smoke independence contract passed; final density/contact tests 3 cases /
  43 assertions passed. Installed Desert smoke capture exits 0 and still shows
  drifting smoke and its ground shadow. This fixture already had shadows before;
  it does not establish acceptance for every reported missing-shadow situation
  or zero cloud strength through the interactive settings.
- Local-light specular implementation pending acceptance: separate raw RGB
  appended to WgrDraw3D (272 -> 288 bytes; both size assertions and automatic
  layout hash handshake), GPU Material 112 -> 128 bytes, retained/per-draw parity.
  Sun highlight unchanged. Local highlight reuses existing attenuation and
  spot/cube occlusion, no duplicate shadow taps; diffuse-only callers retain
  their wrapper. Build passed before adding the source contract; committed
  build, shader contract and installed GLOSS comparison still pending.
- 74369a27: matched EXE/DLL built, pushed and installed; local-specular source
  contract passed, full Showcase harness exits normally. Room GLOSS panel now
  has visible warm highlights absent in the previous image. Bright room edge
  bands remain. Exact Everon road camera still has no blue glass sheen.
- Snowflake legibility follow-up: diagnostic reports roughly 500 drawn flakes
  but the captured image shows virtually none. Replace additive Flare0 sprites
  with a 32-square mipmapped white/soft-alpha particle texture, same count and
  geometry. Texture releases on world/particle reset. Build/test and installed
  comparison pending; accumulation roof exclusion remains a separate open item.
- 173733a4: built, 32 Rain/Snow/Weather cases / 38063 assertions passed,
  pushed and installed. Flock visibility now confirmed, but the fixed-exposure
  scene shows dark particles: legacy RGBA8 sprites cannot carry the HDR sky's
  higher radiance. Follow-up adds an explicit precipitation radiance gain
  (negative 2D fog lane, ABI version 10 on both sides; normal sprites unchanged)
  and hemisphere-averaged daylight gain, plus texture release at world cleanup.
  Build and installed image acceptance pending. This is approximate flake
  illumination, not per-particle shadow/local-light evaluation.
- 29237c7d: committed build, 12 Rust ABI tests and installed snowy mission
  passed. White flakes are now visible at the same fixed exposure where the
  additive baseline showed none. Clean exit after texture lifecycle change.
- Snow accumulation shelter mask in progress: cached topmost shell voxel per
  building model, 0.5m block queries in the existing 64m snow upload window.
  Includes static and mission-spawned buildings; tilted instances use cached
  voxel segment queries. No new collision rays or GPU allocations. Tracks are
  not erased from persistent storage. `POSEIDON_SNOW_SHELTER=0` ablates the mask;
  `POSEIDON_SNOW_TRACE=1` logs its CPU cost. Initial build passed after const
  qualification correction; final tests and installed indoor A/B pending.
  Limits: valid fire/view shells required, near-window only, no roof-top snow
  geometry, and bare-ground collision remains unchanged.
- a52b82e4 installed shelter comparison completed normally. Indoor floor is now
  bare, versus white snow before; sampled steady mask cost 43-234us in this
  Showcase run. 17 unit cases / 9940 assertions passed. Bright edge bands persist
  with room lamp off; disabling CSM lights the entire room. Shadow bias/coverage
  investigation continues. This does not diagnose the user's unspecified crash.

## Additional owner requests, still open

- RTX 3090 Ti black menu: supplied 8c1be95 log explicitly reports DLSS inactive
  (missing snippet DLL), then invalid 8x Rgba16Float MSAA without enabled
  TEXTURE_ADAPTER_SPECIFIC_FORMAT_FEATURES. Device feature negotiation fix and
  enabled-feature fallback tests pass (2); temporary workaround edit
  `%APPDATA%/CWR/graphics.cfg` to `msaaSamples=4;` while game closed. Profile
  overrides WGR_MSAA. With actual profile 8x, installed a52b82e4 reproduces the
  exact tester validation error locally with DLSS disabled. Fix 3d662192 awaits
  installed verification. First env-only run silently stayed 4x, not evidence.
- Destroyed-building glitches: investigate retained geometry/pose invalidation;
  no reproducible scene supplied yet. Zeus torch cone not functional.
- Continuous cursor terrain raise/lower brush; default small explosion craters
  only if measured performance supports it, not the full-rebuild LAB prototype.
- Strong blizzard, snowfall-linked overcast without forced thunderstorm.
- AI: radio must not delay commands; assess overlapping radio, interrupt long
  binocular use on movement; bounded tactical/team coordination improvements
  without breaking campaign orders. Not implemented yet.
- Multi-island OFP world: implemented as a separate flat five-island composite;
  see the Fusion entry below for the installed-test status and limitations.

- a672c93e: installed 8x MSAA/DLSS-off A/B now renders the same Everon scene
  instead of black; no sample-count validation errors, exit 0. Tester hardware
  retest remains external. Both safety refs updated.
- Room ablations: zero depth bias leaves the band, zero normal offset reduces
  side leak, zero PCF shrinks fringe but main strip persists. 30m shadow reach
  with 40m fade is not a valid quality comparison (fades room shadows away).
  Visible Showcase P3DM faces appear reversed relative to CW front convention:
  under the roof the sun-facing top is visible. Reverse visible LOD only;
  collision/fire/view winding unchanged. Three asset tests pass; installed
  visual comparison pending. No global shadow tuning changed.

## Host stability pause, 12:22 local

Owner reports repeated whole-Windows freezes with git.exe access-violation dialog.
Read-only Windows event review: repeated Git 2.44.0.windows.1 crashes
(c0000005 in msvcrt.dll and c00000fd), plus audiodg, NVIDIA container,
webthreatdefsvc and taskkill failures around 12:05-12:07; unexpected shutdowns
recorded for 11:22:42 and 12:07:12. Last Showcase log ends normally at 12:02:49.
No Git/build/game processes running after reboot at inspection. Root cause NOT
established; do not blame DLSS, hardware or RAM based on these symptoms alone.
Builds, Git calls and runtime tests paused pending stability diagnosis. Future
build parallelism must be reduced from 10 on this 16GB host. No settings/driver
changes made. Root C: has about 15.4GiB free at cleanup start.

Owner authorised temporary-folder cleanup. Archive old root test folders to
D:/CWR-Archive/2026-09-09-temp-cleanup, SHA256-verify every file before removing
the C: duplicate. Do not remove .tmp-dlss-sdk (Deploy.ps1 dependency), current
worktree test evidence, Cargo.lock, or .tmp-perf-modernisation (large hardlink
set, logical size overstates space reclaim). Cleanup script in .tmp-bulb.

Torch cone investigation found concrete disconnected control: SetAngle changes
_angle, but LightReflector::GetDescription hardcodes phi/theta. Fix not yet made;
preserve existing vehicle headlight cones when connecting explicit dev overrides.

Post-reboot inspection of already-written room-lit.png from the bcf02269 addon
test REJECTS the winding experiment: panels now black/back-facing and edge leaks
worse. Do not count it as a fix. Revert the visible-winding experiment before
resuming Showcase acceptance; no new runtime or install during stability pause.
Currently installed EXE/DLL remain a672c93e; addon is bcf02269 experimental.

## Resumed with owner's approval after disk cleanup

Six old root test folders archived with per-file SHA256 manifests on D:;
C: free space rose from 15.4 to 23.7GiB. A read-only check found Codex itself
launching background Git rename-detection diffs; no attribution of the historical
crash is established. No new Application Error in the last 15-minute check.
Resume builds at two jobs only, CARGO_BUILD_JOBS=2, runtime serially.

Reverted the rejected visual winding change. Torch dev SetAngle now reaches the
hardware description only when explicitly called; constructor-only vehicle
cones keep legacy values. Invalid angles are ignored. Procedural color NOHQ
normal upload now packs X into A, Y into G, clears R (no fake POM height).
Parser RGB values and ordinary colour uploads unchanged. This corrects the
flat normal's previous X=1 tangent tilt, a concrete additional room-lighting bug.
Build passed; 11 procedural/light cases / 650 assertions passed. Installed room
and torch-width comparisons pending; no full acceptance claim yet.

- f0cb045f installed: original Showcase winding restored, normal upload fixed.
  Full harness exits 0; room point light illuminates surfaces but sunlight edge
  strips persist. Torch narrow 0.12 vs wide 1.1 captures at identical camera,
  strength/exposure clearly change footprint; both exit 0. Explicit cone path
  accepted, ordinary vehicle constructor profile deliberately unchanged.
- CSM experiment: replace one-reference hardware bilinear comparison with four
  texel depth loads and receiver-plane-corrected references before interpolation.
  Removes blanket slope-depth subtraction that can skip thin blockers. Same tent
  footprint, more shader instructions; performance NOT yet accepted. Shader
  composition and two other shader tests pass; build passed. Installed A/B next.
- Snowstorm: separate 0.25-8x flake budget (default 1), capped at 7200 particles;
  button chooses 8x / 0.1m per minute. Snow-induced minimum rendered overcast
  0.75-0.95 does not modify mission rain/thunder state; no new thunder forcing.
  POSEIDON_SNOW_TEST_FLAKES seeds repeatable dense checks. Combined snow/weather/
  light/procedural tests: 45 cases / 38727 assertions. Runtime/performance pending.

### Installed 40fa7d5e follow-up

- Two installed snow captures exit 0: sep9-storm-1x and sep9-storm-8x in
  .tmp-bulb. Eightfold density is visibly stronger, about 7200 vs 900 live
  flakes. Late samples: simulation+draw preparation roughly 0.08-0.10ms vs
  0.61-0.75ms CPU. Short GPU frame samples are noisy (about 8-9ms both);
  do not claim a GPU speedup. Default remains 1x. Camera looks at ground,
  so these captures do not visually validate cloud cover; fixed exposure 0.1
  also makes this fixture too dark for final artistic acceptance.
- Receiver-plane filtering removes broad room sunlight bands. A jagged fringe
  remains at the roof/wall junction. Installed InspectRoom run exits 0:
  zero normal offset does not remove it; zero PCF greatly reduces its width.
  Do not globally disable PCF or declare the room lighting complete. Current
  A/B evidence preserved as sep9-plane-filter-before/after-* in .tmp-bulb.
  Matched outdoor performance acceptance still pending.
- Foreign Cargo.lock SHA256 remains unchanged (9A4E74D7...31A2E).

### AI binocular movement, 85c6c78c

AI observation now yields to requested forward/backward speed instead of
reissuing BinocOn throughout the observation timer. Explicit BinocOff uses the
existing lowering animation. Player action handling unchanged. Three binocular
test cases / 14 assertions pass; committed build at two jobs exits 0, pushed to
both safety refs and installed as matched EXE/DLL.

Installed isolated doMove test on Desert exits 0 before and after. Baseline unit
remains near north=3401.6 for two seconds, moves by second 3; changed build reaches
north=3406.7 in the first second and the 3450 destination by second 8. Evidence:
.tmp-bulb/binocular-baseline-valid-* and binocular-after-*. This reproduces a
short delay, NOT the reported 30-second combat case. First harness attempt used
unsupported currentWeapon/animationState script commands and timed out; excluded.
Radio queue decoupling, overlapping voices and coordinated tactics remain open.

- Nogova sky comparison at the owner's Davle pose: snow-off leaves broken clouds;
  8x falling snow produces closed overcast. Both installed runs exit 0. Evidence
  sep9-snow-sky-clear/storm. Overcast is render-only, no mission thunder mutation.
- Max-density Showcase storm run exits 0; protected room floor remains bare and
  no indoor flakes visible in room-lit. Preserved sep9-storm-room-* and log.
- Next change: immediate offline CtxUI/CtxUIWithJoin command delivery while the
  spoken message stays queued. Mission/automatic contexts and network games keep
  existing timing. RadioMessageCommand consumes its serialized transmit flag
  once, so later audio completion cannot deliver twice. Build and 4 focused
  cases / 32 assertions pass; blocked-speech installed runtime acceptance pending.
- 3dcaf4f3 committed build exits 0; both safety refs pushed and matched binaries
  installed. Radio probe holds a text message for 15 seconds. CtxUI unit moves
  6.9m within one second while the channel still reports BLOCKED at second 2;
  CtxMission control stays stationary through second 14, then moves. Both runs
  exit 0 (sep9-radio-ui/mission). UI unit remains at destination after speech
  drains. This accepts offline move-command decoupling only; formation/stance
  messages, overlapping voices and network delivery are not changed here.
- Matched CSM log phases identified using preserved screenshot LastWriteTime:
  room colour pass 2.370-2.371ms before vs 2.389-2.398ms after (two samples each).
  Castle colour pass 3.731-3.877ms before vs 3.713-4.029ms after (four each).
  These small samples rule out a large regression in this fixture, not in dense
  forests or other GPUs. Keep the remaining roof-edge PCF fringe open.
- Terrain brush investigation: installed Box3D headers expose heightfield create/
  destroy/query, no regional mutation API. Current SetTerrain rebuilds the entire
  heightfield and wakes every dynamic body; Landscape::HeightChange also causes
  full GPU height upload via revision. Do not invoke that on every mouse tick.
  Need bounded regional collision/render updates (or an explicit paused editing
  transaction) before accepting continuous painting or default explosion craters.

### Regional collision groundwork for terrain painting

Added optional PatchTerrain: normal worlds remain monolithic until first edit;
editing partitions into 128-cell collision tiles, then replaces only intersecting
tiles, including both sides of shared vertices. Replacements are built before
publication and invalid/nonfinite edits rejected. Tile storage outlives shapes;
world destroy/recreate and switching back to SetTerrain are covered by tests.
Editor input bounded to at most 2049 samples per dimension. Existing normal
SetTerrain callers unchanged. Currently still wakes all dynamic bodies on edit.

47 physics cases / 2083 assertions pass before final origin/scale test addition.
1025-square flat fixture: full collision build 43.3ms, initial partition 44.6ms,
four-tile repeat edit mean 2.63ms (8 iterations). Not a whole-frame benchmark.
Showcase crater now normalizes differing corpus/native grids once, then uses
regional patches; render caches/GPU height upload still global. Installed test
pending. Mouse painting and default explosion craters NOT yet enabled.

af67b8d6 committed build exits 0, both safety refs pushed, matched pair installed.
Final physics suite: 47 cases / 2086 assertions. Full installed Showcase exits 0;
crater and restoration images inspected (sep9-local-terrain-*). First 7x7 patch
includes grid normalization/partition: 383.3ms. Restore uses local patch: 1.52ms
collision/cache initiation. Subsequent renderer height/quadtree work is outside
that timer; do not quote it as whole-frame cost or enable default craters yet.

### Continuous terrain brush

bc3ae8a8 adds experimental Physics-tab raise/lower painting in Zeus freefly,
left-button hold, capped at four updates/second, single-player and mission-local.
Radius is at least 1.5 native cells; no tunnels/save/undo/navmesh relocation.
b4769ede removes a delayed partial release stroke found by installed testing.
Both commits built, pushed to both safety refs and deployed. Installed SDL input
test exits 0: height 29.835 -> 27.8448 -> 24.8738 while held, remains 24.8738 after
release, then rises to 27.8344. Images/log: .tmp-bulb/sep9-brush-release-*.
Top-down images show only subtle relief; numerical acceptance is not a full
interactive usability/performance acceptance. Terrain/water global rebuild stalls
remain. Default explosion craters remain disabled pending renderer improvements.

160311da clips CDLOD bound scans to distinct native samples rather than repeatedly
walking clamped border/ocean texels. Exhaustive reference comparisons cover inside,
outside, crossing-edge and single-sample leaves, both land and ocean semantics.
62 selected cases / 19668 assertions pass; committed build exits 0, both refs
pushed, deployed. Installed brush repeat exits 0 with identical height sequence.
Offshore Nogova capture at 32km east / 8km altitude exits 0 and has continuous
water, no brown replacement tiles (sep9-clipped-ocean.png). No measured whole-frame
speedup claimed; global rebuild/upload and input latency remain performance work.

Freefly lighting follow-up on installed 160311da: Everon night fixture with live
player, two static near/far launches and one continuous scripted camera trip
(near -> ~1km -> >3km -> return) all exit 0. All 40 light candidates remain
selected; distant village pools remain visible and return view stays lit.
Evidence .tmp-bulb/sep9-village-{near,far}.png and sep9-village-travel-*.png/log.
Static comparisons used exposure 0.3 (too dark for subtle diagnosis); travel uses
fixed 2.0. This does NOT reproduce the reported cutoff, so no speculative radius
or selection change made. Other locations/settings remain unverified.

Destroyed building reproduction on 160311da: nearestBuilding player resolves to
Househospoda_mesto at [5097.49,3990.6,21.676]. setDammage 1 visibly deforms it,
but after the same camera's far trip and return it renders intact again. Saved
sep9-building-collapse-{intact,collapse,wreck,return}.png; harness exits 0.
BeginMeshTL snapshots CPU-posed helicopter meshes but not destruction morphs;
shared dynamic buffers can therefore replace queued wreck geometry with another
draw's vertices. Extend existing per-frame pose copies to non-tree destroyed
objects, retaining the skinned/static exclusions. Installed A/B pending.

5e8b7a7e installed A/B exits 0: sep9-building-isolated-wreck/return.png now both
retain the collapse deformation; adjacent buildings remain intact. This fixes
the reproduced pose replacement, not the aesthetic of the legacy folded mesh.
Follow-up also snapshots destroyed objects whose streamed source LOD is static;
helicopter gating unchanged and GPU-skinned objects still excluded.

0d264e13 committed build/test deployment and repeated collapse trip exit 0;
return image inspected and remains deformed. 17 Object/shadow-caster/CDLOD cases
with 17579 assertions pass. Broader ruin models and many simultaneous wrecks
are still outside this fixture's acceptance.

### Local terrain sky visibility

Each height edit currently invokes the full CPU sky horizon scan. Preserve raw
unblurred visibility and rescan only samples within the changed-height bounding
rectangle plus the full horizon radius and bilinear footprint. Full blur retained
so results are identical; changed dimensions/grid/options still use full scan.
Additional persistent storage: one float per coarse sky-visibility texel.
Four Rust tests pass, including exact comparisons after center/edge/corner edits,
downsamples 1/2/4/8 and unchanged input. Unoptimized CPU 513-square fixture:
full 281.4ms, local 14.6ms. Not release or frame timings. Installed test pending.

895f39c0 committed build exits 0, both refs pushed and matched pair installed.
Installed brush test exits 0: 29.835 -> 23.8911 -> 16.2877 while held, remains
16.2877 after release, then rises to 22.8487. More updates now fit the same hold
period than before; capped brush time intentionally does not catch up stalled
frames. Image inspected (sep9-brush-skyvis-lower.png). This is improved interactive
throughput, not a controlled whole-frame GPU speedup claim. Cargo.lock hash intact.

Full installed Showcase on 895f39c0 exits 0, including crater and restoration.

### Analytic local-light glow

Optional graphics interface replaces local WGPU Flare0 rendering with analytic
radial halo/HDR core in the existing 2D additive batch. Sun/secondary flares and
GL33 remain unchanged. Existing local source occlusion and angular sizing retained;
core uses one HDR quad rather than eight overlapping quads. No new texture or
fullscreen target; existing white fallback bound. New ABI capability bit protects
against an older DLL interpreting the vertex marker as ordinary fog.
POSEIDON_ANALYTIC_GLOW=0 retains the legacy path for A/B. Native build and two Rust
shader/ABI tests pass. Installed visual comparison pending; not yet acceptance.

ea5643e5 built/pushed/deployed, final shader/ABI tests pass. Installed matched
1280x720 night Jeep A/B exits 0: sep9-glow-bitmap/analytic.png inspected. Bulb ROI
x615..663/y359..378 peak goes 129 -> 255; pixels over 180 go 0 -> 14. Both lenses
now read as lit; lamp halo is smooth and brighter. Last seven whole-GPU samples
median 8.575/8.761ms; noisy separate launches, not an isolated halo-cost benchmark.
Separate sep9-analytic-occlusion run exits 0; blocked sources still report occ=0,
including house blockers. Broader subjective flare tuning remains possible.

### Smoke room lookup follow-up

Baseline ea5643e5 Nogova hasic_zbroj, source 7731.6/25.3/4421.4, 8x sprites,
wind 18/250/0.2, 30s exits 0. Late simulation ~11-15ms, ~3900 particles;
most particles have room=-1 and issue seven-ray containment sweeps every tick.
The interior grid is not even built until late in this run. Fallback lookup uses
Object::IsInside (solid convex components), which cannot reliably classify the
air in a hollow building. Pending fix directly queries nearby BuildingInterior
room grids, retaining the validated cache and original 25m search extent/order;
skips other object classes and rejects destroyed buildings' stale intact rooms.
POSEIDON_SMOKE_BUILDING_FILTER=0 retains old fallback for comparison. No wind,
downwash, particle cap, containment cadence or collision response change.

Native two-job build exits 0; 36 Smoke/BuildingInterior/Wind/Airflow cases pass
28,591 assertions. Installed A/B remains pending.

252c9cda built/pushed/deployed; same installed 30s sprite run exits 0. Late samples
2.027-2.171ms at ~3900 particles, ~3300 correctly labelled indoor versus baseline
11-15ms and mostly unlabelled. Ray counter ends ~4.0M versus ~25M. Both exterior
captures inspected, no gross escape plume; this does not prove thin-wall sealing.
Grid creation for unrelated neighbouring houses can hitch (one 85.5ms build).
Follow-up rejects points outside the existing all-LOD bounding sphere plus 2m
scaled grid margin before creating an interior; no change to actual room tests.

f94eb275 built/pushed/deployed. Same 36 targeted tests pass. Installed 30s volume
run exits 0, late sim ~2.0-2.3ms. Needed hasic_zbroj grid costs 30.5ms once;
neighbouring 85.5ms bake is gone (other reached types 3.3/10.2ms). This does not
eliminate all cold-grid hitches. Exterior volume image inspected: no large plume
bleed visible, but an exterior view alone cannot certify every thin wall/roof.

### Player head-shadow acceptance

Installed f94eb275 head.noe, first-person look-down at 17:00, three orderly exit-0
runs. sep9-head-detail-sharp.png clearly shows a complete helmet/head, shoulders,
rifle and legs. Diagnostic uses PCF=0 and shadow distance=40m at unchanged 2048
resolution; normal runtime reports 1200m and PCF=1, where the same silhouette is
coarse/soft. External view also inspected. Thus the exterior-caster LOD fix is
visually verified for this SoldierWB/player fixture, not all units/vehicles.
No production shadow-distance/resolution/filter default was changed. Diagnostic
uses a private test profile; normal settings remain unchanged.

### Far-foliage rays investigation / active-game barrier

Source inspection: the post-water replay happens after the godray composite.
shader3d adds surface_scattering only when its translucent override is true;
gfx3d sets that override from final Alpha blend mode. EngineWgpu can promote
legacy plant/net masks to Opaque even in a late blend/whole-alpha revisit. Such
late opaque draws have no surface scattering. This is a plausible mechanism,
NOT yet the reproduced cause of screenshot 073829; do not brighten all trees or
move rays back after transparents (the documented sign/cockpit regression).

The requested new forest baseline was refused by the game lock: an outside-lock
OpenPoseidon.exe launched normally at 17:15:35 (PID 61712, no harness arguments).
It was not stopped or modified. No build/deploy was run concurrently. Next:
capture late foliage draw classification and rays on/off at a verified forest
pose after the installed-game slot is free, then apply a narrowly routed fix.

### Fast-freefly terrain temporal follow-up (pending validation)

Owner reports terrain ghosting during fast freefly. Source inspection found that
temporal.wgsl fs_velocity reconstructs jittered scene depth using an unjittered
inverse camera without removing current projection jitter from the pixel NDC.
Pending patch carries the existing apply_jitter offset in unused velocity-pass
misc.xy and subtracts it before reconstruction and unjittered motion calculation.
No new texture, draw, sampling loop, global history reset or shader ABI size.
Added Rust projection-reference regression for translated/rotated camera with
nonzero jitter at two resolutions. Not yet compiled/run or installed: normal
OpenPoseidon PID 39868 is active and remains untouched; no concurrent build.
Needs targeted temporal/shader tests, committed build and installed moving-camera
DLSS A/B. This is a concrete temporal input correction, not yet proof that the
owner's complete terrain trail is fixed. Terrain LOD/disocclusion and float-depth
precision remain separate candidates; cloud motion history remains unresolved.

### Nogova cloud / ocean horizon report

Owner reproduction: freefly [8568.71, 4139.18, 305.40], azimuth 231.0,
elevation -1.6. Clouds appear to enter the distant water horizon. Pending visual
reproduction; outside-lock game PID 39868 remains active and untouched.
Source candidate: cloud_interval clips against a spherical planet while the
rendered ocean uses a flat sea plane. fs_cloud bounds against opaque scene depth;
the water pass is depth-read-only, so this is not the water surface distance.
A shallow downward ray can miss the sphere yet intersect the flat ocean, allowing
far clouds to composite over water. Upsample edge leakage is a separate candidate.
Do not hide this with fog or alter reflections blindly. Next test the exact pose
with DLSS off/on and bounded water-surface cloud march; use actual GetSeaLevel,
not hardcoded zero, and preserve cloud foreground/underwater/inland behaviour.
No horizon shader change has been made or claimed verified yet.

### Showcase castle gameplay contact follow-up

Owner confirms grenades/missiles did not break the castle and the player walked
through it. Root cause: probe rendering/rigid bodies were outside legacy object
collision and radial damage; the kinematic proxy was explicitly one-way.
Added opt-in showcase player swept block contact/slide, shell/grenade/missile
block hits and a bounded radial impulse bridge from ExplosionDammage. Fused
grenades retain bounce/fuse; scenery hits nearer than a block remain authoritative.
Rotated blocks use live physics orientation, not their original castle grid.
This is a conservative capsule/slab sandbox bridge, NOT a general character
controller or networked rigid-body ownership migration. No AI/vehicle controller
replacement; ordinary missions without the showcase are inert.
Build passed; 19 assertions in two new contact/blast tests passed. Installed
walk/gate/grenade/rocket checks pending. Added Test-Showcase.ps1 -CastleOnly.
The pending temporal jitter correction also passed all 8 temporal Rust tests,
including the headless velocity shader and translated/rotated camera reference.
Game is closed again; runtime validation can proceed with the game-folder lock.

Installed acceptance on 3e8ecbde: Test-Showcase.ps1 -CastleOnly exited 0.
Four seconds holding forward stopped the player at northing 3563.26, before
the rear wall face 3563.61. Separate gate traversal passed. GrenadeHand
(indirectHit 18/range 7) reached 332 bodies and visibly opened a breach;
LAW (150/2.5) reached 99 and collapsed a larger wall section. Captures:
build/showcase/smoke/castle-walk-blocked.png, castle-grenade.png, castle-rocket.png.
49 physics cases / 2105 assertions passed. First front-wall fixture was invalid:
manual view heading caused a diagonal path through the gate. The corrected
fixture tests the continuous rear wall and the gate separately; not a code ablation.
Cargo.lock remains unchanged from the owner's foreign edit.

### Ocean cloud clip experiment after castle acceptance

Captured the owner's exact Nogova freefly pose on 3e8ecbde:
.tmp-bulb/nogova-horizon-before.png. Added a main-view flat-ocean ray bound to
fs_cloud, using the renderer's actual water sea_level and a spare SkyUniform lane.
Both cloud layers share the bound; foreground cloud and earlier opaque depth
remain authoritative. Planar reflection uploads explicitly disable the bound;
underwater/no-water-camera paths encode zero. No additional pass or texture fetch.
WGR_CLOUD_OCEAN_CLIP=0 is the A/B control. 39 sky tests passed (1 ignored), including
WGSL validation and real-device pipelines. Installed A/B still pending; no claim
yet that this closes the reported horizon appearance or temporal cloud trails.

Installed abd8c50f A/B at the exact pose completed with exit 0 in both arms:
.tmp-bulb/nogova-horizon-after.png and nogova-horizon-control.png (clip=0).
The thin bright bands just below the flat sea edge disappear with clipping and
return in the same-build control. Cloud above the horizon, island foreground
and reflected sky remain visible. Last six GPU cloud-march samples: median
1.009 ms enabled versus 1.031 ms disabled (ranges 0.970-1.320 / 0.992-1.514 ms).
This small single-fixture comparison shows no evident extra cost, not a general
benchmark. DLSS Quality was active (858x482 -> 1280x720). Cloud grain/history and
low-resolution shapes near the horizon remain separate, unclosed quality issues.
Latest installed game is abd8c50f, including the accepted castle contacts and
the tested depth-jitter motion correction. No user settings or Cargo.lock edited.

### Falling snow: flake shape, wind and fast-camera population

Owner reports round balls and outrunning precipitation in freefly. ae59e548
replaces the radial disc with a feathered six-arm 64x64 alpha mask, variable
1.1-2.7 cm sprites, rotation and tumbling aspect. Calm terminal fall is now
0.36-0.94 m/s with gentle horizontal flutter; wind response is 1.0 (previously
0.6), with retained velocity relaxed at 3/s. No gameplay RNG consumption.

Root cause of the missing field: only spawning above the camera, while old
off-camera particles survive until ground contact and occupy the capped pool.
Snow now removes particles outside the camera-local cylinder and replenishes
camera losses throughout its visible vertical column. Retained particles stay
world-space; camera velocity is not added to them. Failed shelter spawns retry
at the normal rate, not a full pool per frame. Existing spawn shelter and
room/roof/wall sweeps remain enabled. Rain behaviour is unchanged.

Installed ae59e548: calm and 18 m/s wind/8x density runs exited 0, including
a ~5.8 km camera move in four seconds and an altitude change to 900 m.
Calm pool remained 700, storm 5600, including during flight; hundreds/thousands
of visible quads persisted. Fast-flight CPU spawn+simulation+draw samples were
roughly 0.14-0.30 ms calm and 1.1-1.8 ms at maximum storm density, not a complete
GPU benchmark. Captures/logs: .tmp-bulb/snow-calm-* and snow-storm-*;
reproduction harness .tmp-bulb/snowflakes.ps1. Near flakes show arms/variation;
distant subpixel flakes can still appear as points, especially with DLSS.

e0683049 additionally draws/replenishes snow at zero simulation delta, without
advancing fall/tumble. Installed pause run exited 0: time stayed 22.865 for two
wall-clock seconds and flakes remained visible (.tmp-bulb/snow-pause-paused.png).
14 Rain/Snow test cases / 9932 assertions pass on the final build. Installed
e0683049, both safety branches pushed; foreign Cargo.lock hash unchanged.
Ground snow appearance/geometry was not changed by this precipitation fix.

### Map Editor tab and terrain-paint instructions

94d96586 moves the brush from Physics into a dedicated Map Editor tab with
Raise/Lower radio buttons, radius/speed, effective terrain-grid radius, explicit
hold/release instructions and a Start painting button that enables Zeus freefly,
arms the brush and hides the panel. A viewport banner identifies the active
mode. Brush-only reset is explicit and also part of master Reset settings;
neither reset erases the user's terrain. Multiplayer/no-world controls disabled.
No new-map generation, terrain export, undo stack or building/AI relocation
has been added or advertised as available.

f486ff37 moves Start painting above the fold after inspecting the first capture.
Installed final panel is fully visible at 1280x720 in the default-sized window:
.tmp-bulb/map-editor-final-panel.png. Two installed brush runs exited 0. Final
height checks: 29.835 -> 23.8911 -> 16.8503 while held, unchanged 16.8503 after
release, then raised to 19.4914. .tmp-bulb/brush.ps1 supplies the mouse-hold test;
it switches modes via the existing diagnostic command, not a UI-button click.

Build passes. Targeted panel/terrain tests: 2 cases passed, 1 existing broad
panel-source guard failed (6 assertions: old raw ImGui controls/hover calls in
DebugOverlay outside this change). Confirmed e.g. Probe basis raw Combo already
exists in f37fb9bb. New MapEditorTab.cpp is included in that guard and uses the
filter wrappers; no failing controls originate there. Do not report the entire
suite green. Installed f486ff37, pushed to both safety branches; Cargo.lock untouched.

### Intermittently stretched NPC faces

Reproduced the owner's September 9 07:41 face distortion on installed f486ff37.
An existing SoldierWB looked normal initially; loading mixed infantry classes
(including SoldierWMedic) reused the same cached (model, skeleton) WeightInfo.
SetupFaceBoneRig ran again without a preceding Skeleton::Prepare, found every
face vertex already claimed, reported `groups bound=0`, and set _faceBoneBase=-1.
The vertices remained bound to synthetic bones, which were then left at identity
instead of following the head. A seated close-up reproduces the stretched mouth:
.tmp-face002/before-close.png, with the duplicate setup in before.log.
The 80-unit mixed-class spawn is a trigger, not an established NPC-count limit.
The harness's cargo commands did not reliably seat units inside the helicopter;
the decisive close-up explicitly positions a soldier and uses the Cargo animation.

Setup now preserves an already built face rig. Skeleton::Prepare is incremental:
Init retains entries and AddSelection skips previously registered selections.
The regression test creates a real weighted face selection, repeats setup 100
times, and verifies an additional Skeleton::Prepare also preserves the binding.
No added per-frame renderer work, no disabled facial animation, no AI changes.
Installed bb6d606c passed the identical mixed-class spawn and seated close-up,
exit 0. .tmp-face002/after-close.png shows the normal face, versus the visibly
stretched mouth in before-close.png; both inspected. after.log retains one
successful binding per model and no duplicate `groups bound=0` reset. Harness:
.tmp-face002/repro.ps1 (-Name before/after), via scripts/with-game-lock.sh.
The original campaign cutaway was not replayed; this is an independently
reproduced matching defect, not a claim that every possible face bug is closed.
13 targeted face-rig/Shape cases, 403 assertions pass on the committed build.
Both safety branches pushed; installed stamp bb6d606c; Cargo.lock hash unchanged.

### Optional rocket impact terrain craters

Dev Tools > Map Editor exposes Rockets create terrain craters, radius 8-100 m
(default 12), depth 0.1-8 m (default 1), initially disabled. A separate reset and
the master settings reset disable/reset the option without undoing terrain.
Only explosive Missile ground impacts without a directly hit object enter the
existing bounded PaintTerrain path, after normal damage/effects. Single-player
only; near-ground tolerance 2 m, submerged sea terrain and airbursts rejected.
The existing 1.5-grid-cell minimum radius, 2049-grid limit, collision patching,
mission-local lifetime and no building/AI path relocation limitations apply.
No claim of small high-resolution craters on coarse original OFP heightfields.
The first impact can pause for physics terrain creation, so this remains opt-in.
Diagnostic triRocketCraters on/off/reset uses the same settings as the panel.
Build and 4 crater/terrain-patch cases (63 assertions) pass. Installed 6e71d593
LAW off/on/reset test exits 0: centre height 29.835 -> 29.835 (off) -> 28.835
(on) -> 28.835 (reset/off). The rocket is aimed down from measured terrain+15m;
the earlier absolute 15m spawn was underground and correctly rejected, not a
valid ground impact test. A config-query diagnostic timed out and was removed
from the harness. Warheads are gated by indirectHit, not the explosive fraction.
Log records ground impact [9700,29.835,3600], blast=150, changed=true.
.tmp-bulb/rocket-test.ps1 and rocket-test.log reproduce the acceptance; screenshots
rocket-crater.png and rocket-panel.png. The panel's new section is below the brush
instructions and requires scrolling at the default panel height. UI activation
uses the same settings as tested triRocketCraters; actual checkbox clicking and
large salvos were not benchmarked. Installed 6e71d593 pushed to both safety branches.

### OFP Fusion: five neighbouring original islands

713d1378 adds a local-data exporter/builder/installer and FusionTour.FusionOFP.
Everon, Malden, Kolgujev, Nogova and Desert Island share a 51.2 km square OPRW3
world, without scaling or rotating their terrain and 407848 original placements.
Source tiles have at least 3.2 km additional ocean separation. Exact float height
samples, geography/sound/random fields, mountains and 1092 ground textures are
retained; object IDs are reassigned uniquely. Generated retail data stays ignored
in build/fusion and is installed only to AddOns/fusion_ofp.pbo, not redistributed.
Original worlds are untouched. Mission coordinates and IDs from original campaigns
are not automatically migrated; only island labels and a translated Everon ILS
are configured, not all town/airport metadata. README has build/start instructions.

Initial installed tests loaded all five islands' world data, but did NOT produce
a playable frame: first config class/WRP basename mismatch (fusion vs FusionOFP)
prevented mission init. Fixed by matching the WRP basename and inheriting Eden's
world config; the second run reached InitVehicles ok=1 and DisplayMission, then
crashed in std::_Allocate_manually_vector_aligned at TerrainWgpu::UploadIfNeeded.
At terrainGrid=6.25 this world has 8192 squared samples: the redundant float vector
needs 256 MiB plus STL alignment, exceeding Jimbo's 256 MiB single-allocation cap.
The upload now borrows Landscape's existing contiguous float array. The FFI copies
to GPU staging/owned Rust storage synchronously, so no borrowed pointer escapes.
The allocator safety cap remains unchanged. Row-major view test covers subdivision,
negative heights and a subsequent height edit. Runtime acceptance is pending the
new binary build/deployment; .tmp-bulb/fusion-test.ps1 visits all five islands.

Follow-up acceptance on installed 4f1bd59c closes that pending runtime check.
8d96c726 avoided the allocator crash, but screenshots still had no terrain.
8182acae/521b597d made the previously swallowed Rust panic visible: shadow_mask_dims
called clamp(8192,4096), which panicked with "min > max" after heightmap staging.
0fe5ec87 caps shadow dimensions independently of source size; the shadow texture
stays <=4096 while the 8192 source retains full detail. Added regression test
covers 2048/8192/16384 sources, device caps, zero scale and saturated scale.
4f1bd59c also bounds automatic subdivision by an 8192-side float-array budget;
102.4 km at preferred 6.25 m safely uses 12.5 m, logged explicitly. Original
256-cell worlds retain their requested 6.25/3.125 m settings. The low-level
subdivision loop also refuses growth beyond that ceiling, including cheat calls.
This does not make >8192-side authored data or arbitrary land-cell grids safe.

The installed five-island test now renders terrain, water and original placements.
All five local captures plus the common overview were inspected; helicopter visits
were confirmed with getPos. Heights: Everon 16.335, Malden 269.55, Kolgujev 449.28,
Nogova 22.4874, Desert 29.835. Sampled working set 2201-2300 MiB; clean exit 0.
The built-in Visit actions appear on the start screen; UI-clicking them and a
complete continuous flight between every pair were not separately automated.
Original tile-like forests and visible underwater source-boundary lines remain
visual limitations, not claims of a redesigned ocean/vegetation system here.

Owner's additional >50 km size question: build_size_probe.py/Install-SizeProbe.ps1
generate/install an independent 102.4 km, 2048-cell, zero-object diagnostic world.
Installed 4f1bd59c started its mission (not just editor), uploaded the full 8192
heightfield at effective 12.5 m, and visited [10000,10000], [51200,51200] and
[92000,92000] with camera and player helicopter. All three centre heights are
250 m; sampled working set 1708-1724 MiB, terrain ready=true, exit 0. The far
island capture was inspected. Height upload/sky visibility took about 25 seconds
on this host, so this is a feasibility test, not a claim of instantaneous loads.
No dense 100 km world, large AI battle, MP, save/load or exhaustive collision test;
no measured absolute maximum. A densely populated larger world still calls for
streaming/budget work rather than raising all limits. The separate 50 km visibility
preference is not the terrain-area limit. Original Desert crater off/on/reset
regression also passes on this binary: 29.835 -> 29.835 -> 28.835 -> 28.835, exit 0.
16 native heightfield/subdivision/CDLOD/ID cases (18606 assertions), the new Rust
shadow-mask case, and 3 fusion-builder Python cases pass. Cargo.lock unchanged.
scripts/Test-Fusion.ps1 provides a durable private-profile version of the runtime
harness; screenshots/logs go to build/fusion/acceptance, with -Large for size probe.
Both durable harness modes were re-run with a freshly generated private graphics
profile on installed 4f1bd59c: exit 0, all expected heights and vehicle positions,
heightfield ready=true. Fusion working set 2193-2261 MiB; sparse probe 1772-1820
MiB. Fusion overview and 92 km probe capture inspected again. No claim that the
whole native/Rust suite or older unrelated visual roadmap requests are complete.

### Terrain brush radius and sideways cave request

b51f877d adds Show brush radius at cursor (default on) in Map Editor. The free-fly
overlay follows the ground with 96 segments, a centre marker and effective-radius
label. Green=raise, amber=lower, red=not editable. Picking/radius are shared with
PaintTerrainAtPixel, including the minimum 1.5-cell radius and editing bounds.
It is an editor overlay, not depth-tested geometry. Existing settings resets
restore the toggle; no extra per-frame work when the brush/preview is disabled.
The panel now explains the 2049-sample painting limit and that height painting
cannot create a roofed sideways cave. No misleading cave tool was exposed.

Installed b51f877d acceptance: .tmp-bulb/brush.ps1 -Name brush-radius -RadiusView,
using the existing Desert test mission and a 220 m free-fly camera. Amber/green
64 m ring captures inspected. Hold/release heights 29.835 -> 23.1027 -> 16.0476,
unchanged 16.0476 after release, raised to 22.0798; exit 0. Five targeted native
cases pass (83 assertions), including the radius/paint-footprint test. Shutdown
logs still contain four pre-existing LockCache unlock warnings, also present in
the earlier original-map rocket acceptance before this change; not a clean-log
claim. Off-centre physical mouse motion and high-DPI displays not separately
automated. Both safety refs contain the change; Cargo.lock untouched.

Sideways caves remain unimplemented, not a renderer-only workaround. The actual
heightfield, infantry movement, projectile and PhysicsWorld integration points,
plus a staged local 3D mesh/collision experiment, are documented in
decisions/TERRAIN-CAVES-AND-BRUSH-20260909.md. Rendering a hole without replacing
its solid hill collision is explicitly rejected as an incomplete implementation.

## Default content packages (2026-09-10, started)

The owner authorized continued implementation, with the next overall report
only after the outstanding scope is handled. Incremental evidence lives in
[DEFAULT-CONTENT-PACKS-20260910.md](decisions/DEFAULT-CONTENT-PACKS-20260910.md).
The first increment is compatible-package discovery and existing-loader mount
ordering, not delivered replacement materials or Jeep FFV. Earlier outstanding
terrain caves, far-view and streaming work is not closed by this increment.

Follow-up: an own procedural road is packaged and automatically mounted on the
installed 24ac4dfe build. Corrected PBO normal resolution is verified by binding
logs and a controlled Albedo/Full image difference. The supported client build
now generates the first archive automatically. Three-surface acceptance,
roughness/terrain-normal integration and Jeep FFV remain open; this is not a
completed default-content release. See the decision's run IDs and limitations.

Later verified increments: exact asphalt, Eden sand and field-track mappings
are packaged in 0.1.3, and the existing terrain normal slot now accepts legacy
AG normals with matching cell clamping and a flat far-Satmap early-out. Field
tracks preserve the original transparent grass centre. These are three
candidates, not photorealistic/material acceptance: roughness binding and the
full lighting/weather/LOD comparison matrix still remain open, as does Jeep FFV.

Streaming safety follow-up: typed prepared payloads no longer consume the wrong
kind and RGBA retirement releases outside the store mutex (6 cases/68 checks).
A native measurement exposed an independent long-mission-name buffer overwrite;
d8db97c5 bounds/rejects these identifiers without changing the header ABI and
passes 17 loader cases/84 checks. The installed native 4 km lifecycle run now
passes with 138 driver-memory samples. Details and invalid-run exclusion are in
the three-track decision. This is neither a completed loading pipeline nor
forest/building HLOD, which remain separate open tasks.

Further September 10 measurements: bounded native tint-decode reuse is implemented
only as an OFF-by-default experiment. 144 runtime hits were byte-identical, but
five valid measured runs do not show a stable tail/FPS/memory improvement; one
additional reuse-OFF run hung at shutdown and is explicitly excluded, not hidden.
The native harness now rejects timeouts and incomplete shutdowns even if a
screenshot/metrics file exists. Native PAK reads no longer write a shared archive
error from worker threads; active conversion resources are retired outside the
queue lock. Installed ee43ac8a passes the native route and strict shutdown checks.
The intermittent hang remains an open diagnosis, and material/FFV/far-field
completion is not implied by these safety increments.
