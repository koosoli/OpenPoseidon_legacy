# September 9 visual follow-up

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
