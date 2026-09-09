# September 9 visual follow-up

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
- Multi-island OFP world: prefer flat neighbouring islands before a globe;
  investigate world data/coordinate/import constraints before claiming support.

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
Runtime acceptance of the fix follows below after the installed A/B check.
