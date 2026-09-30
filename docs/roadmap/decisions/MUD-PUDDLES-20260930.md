# Rain puddles on mud — bounded first slice, 2026-09-30

Oliver explicitly prioritised mud puddles and authorised parallel implementation from
`codex/streaming-residency` (`ed452be8`). The roadmap's earlier wet-surface ticket excludes
puddles; this owner request authorises a focused extension rather than a broad hydrology
or terrain-accumulation framework. The integrating root owns full build/deployment/runtime
acceptance. This decision records source checks and the bounded installed results below.

## Implemented behavior

- Cosmetic standing-water patches on source materials explicitly identifiable as mud,
  dirt, soil or clay. Eligibility reads the **leaf**, not parent folders or pixel color.
  Hard/grass/sandy/forest/unknown mixtures are rejected. Native Enfusion eligibility is
  recorded from the semantic palette **.emat leaf**, including CamelCase mud variants,
  while the material is parsed. Semantic hard/grass/forest/beach compounds veto admission
  without depending on separators or casing. An inherited generic Dirt BCRMap cannot
  override BeachGrass/Forest/unknown identity; the generated GPU texture is anonymous.
- Legacy CfgSurfaces requires all four preblended quadrants to be eligible. Mixed and
  unknown tiles stay dry. Authored Arma terrain slot bits use the existing per-pixel LCA
  weights, normalised over **all selected source slots**, including unknown/unbound
  slots. Dropping a rock color texture cannot promote a minor mud share into full mud;
  color/normal composition retains its separate resolved-slot normalization. No extra
  LCA fetch or color heuristic.
- Rain uses the existing captured landscape/effective rain density and simulation clock.
  The single cosmetic saturation value fills with a 30-second time constant in full
  rain and dries with a 180-second time constant. Exact exponential integration makes
  constant-rain results independent of frame rate. Pauses and repeated reflection draws
  do not advance it. A fresh map and a reset mission clock start dry; editing the same
  terrain heightmap does not discard rain history. `WorldInit.cpp` resets `Glob.time`.
- The saturation value controls coverage, not a whole-map muddy material flag. Puddle
  shapes come from fixed world-absolute 6 m and 2.2 m smooth noise. Wetness expands
  connected patches rather than translating or re-seeding them. A footprint fade
  removes them before the finest field becomes subpixel; no animated ripple normal.
- Near-flat terrain only: geometric normal Y fades from 0.995 to 0.9995 (about 5.7 to
  1.8 degrees). All ground below sea level +3.5 m is excluded, fading to eligibility at
  +4.5 m, preserving Tidewater's existing coastal wet sand/residue domain. Snow excludes
  the film. Raw geometric zenith rain reach suppresses darkening and reflection under
  shelter, independently of artistic ambient strength/floor sliders; existing ambient
  shading still uses those controls. The extra one-layer raw map query runs only at wet eligible
  puddle pixels. With that system disabled/unavailable, this is not an independent
  rain-roof collision solution.
- Puddle albedo darkens locally. Flat water-normal Fresnel reflects the existing broad
  SH sky and a broad sun lobe, with terrain/object/cloud shadows retained. No textured
  glitter normal, planar reflection, SSR, new render pass, texture allocation, water body,
  height/collision/nav/grass change, network state or GL33 implementation. Reflection of
  nearby objects and physical puddle depth are deliberately absent from this slice.

## ABI and default preservation

`WgrTerrainParams` remains 88 bytes; formerly zero `_pad1` at byte 72 now carries rain
wetness (the C++/Rust name is retained for existing initialisers; WGSL calls it
`rain_wetness`). `WgrTerrainMaterial` remains 384 bytes; its trailing word at byte 380
is now `puddle_flags`: bit 0 is eligible legacy/native material; bits 8..13 are authored
surface slots. Byte layout and shared grass terrain bindings are unchanged. **Semantics
changed even though the size handshake does not**, so the executable and renderer DLL
must still come from the same committed build. Old matched builds simply do not know
these effects. Default Tidewater Native selection and all existing water profiles stay
untouched. Dry wetness is zero and takes no new noise/sky-reflection work; no measured
zero-overhead or byte-identical-frame claim follows from source inspection.

## Automated checks

- `scripts/Test-TerrainPuddles.ps1`: standalone C++20 policy compiled with clang++,
  `-Wall -Wextra -Werror`; eligible native/Arma leaf names, rejected hard/unknown/mixed
  names, rain filling/drying, pause/duplicate draws, finite inputs, clock/map reset and
  60 Hz versus one-step accumulation assertions.
- `cargo test -p wgpu_renderer --offline terrain:: -- --nocapture`: 16 passed, including
  real Naga terrain shader composition, native ground/AO variants, material/parameter
  offsets, source-mask and weather/slope/shore/snow/footprint contracts, existing sky
  visibility checks. One existing source assertion was updated for the new internal
  `AuthoredCell` mud member; native shading behavior remains covered.
- Admission review follow-up: an explicit 80% rock/unknown +20% mud source mixture is
  rejected with both color slots resident and with the rock/unknown image missing.
  The source helper counts all six mask slots independently of residency; no change
  to the existing color denominator. A roof test also covers raw admission independently
  of zero ambient strength/high ambient floors. The terrain subset now has 18 passing tests.
- Native identity follow-up: the integration-owned installed fixture log
  `build/terrain-puddles/native-fixture-20260930-100657/engine.log` reported 3/22 eligible
  entries and selected `BeachGrass_01.emat`. This invalidated the inherited-BCR admission
  assumption; that candidate is a negative control, not accepted mud. Native eligibility
  now requires explicit palette Mud/Dirt/Soil/Clay semantics and rejects compound beach,
  grass, forest, rock, sand, road/path/trail identities despite inherited dirt textures.
  Standalone tests cover this actual BeachGrass case, native Dirt_01/02/03 identities,
  other loaded Grass/Forest/Pebbles/SeaBed leaves, unknown identity, CamelCase wet-mud
  variants and directory-name isolation. A new installed fixture is still required.
- `git diff --check`: clean. Cargo regenerated an unrelated two-entry CLI dependency
  lockfile difference; that file is not part of this change.

## Installed acceptance owned by integration

Use the matched installed build and game-folder lock from `.agents/SMOKE_TEST_INSTRUCTIONS.md`.
Existing native data is at
`C:\Program Files (x86)\Steam\steamapps\common\Arma Reforger\addons`, with
`POSEIDON_REFORGER_WORLD=worlds/eden`, native objects/streaming enabled, and
`tests/perf/missions/perf_field.eden` (existing native harness).

1. Enable `WGR_TERRAIN_PUDDLE_FIXTURE=1` on one load. The CPU logs material count and a
   every native semantic .emat/BCR/admission decision and a flat, inland,
   native-road-free 3x3 eligible patch as an exact freefly candidate.
   This does not teleport the owner or assert the selected site is visually suitable.
   A roof/building can still make a logged candidate unsuitable; inspect before accepting.
2. Reuse that exact candidate pose/light/native render settings with process-only
   `WGR_TERRAIN_PUDDLE_WETNESS=0` and `=1` for an appearance A/B. `WGR_TERRAIN_PUDDLES=0`
   is the independent master-off control. No water/weather profile is saved by these.
3. Capture grazing, top-down, camera movement/altitude and a ground-type boundary. Check
   patch stability, plausible puddle contrast, absence of fine glints/cell-sized shine,
   and dry grass/rock/sand/road ribbons. Measure the terrain GPU region on identical
   warmed scenes; shader/CPU tests do not establish performance or visual acceptance.
4. Remove the override and use actual rain (`POSEIDON_TEST_RAIN` or the weather controls)
   for at least 30 simulation seconds, pause, then stop rain and observe gradual drying.
   Check a mission restart, snow-covered terrain and an unchanged Tidewater shoreline.

Road **materials** never receive this terrain slice. The surface beneath an authored
opaque road ribbon still renders its source terrain and is normally concealed by that
ribbon; no new ribbon-gap or transparency solution is claimed. The diagnostic candidate
excludes the native road-cell mask rather than assuming a dirt-colored road is mud.

## Installed first-slice evidence

Full matched game builds pass, followed by serial, normal-exit installed captures.
Source `090a513e` was installed with a `+dirty` stamp because status documents and
Cargo's unrelated generated lockfile change were present; rendering code was
committed. Per-run provenance retains the actual stamp, hashes and source HEAD.
The final clean installation is recorded by `DEPLOYED-FROM.txt`; code-identical
documentation/runner commits do not retroactively rename these captures.

Ignored evidence is under `build/terrain-puddles/`. `semantic-fixture-20260930-101738`
logs all 22 native palette material identities and inherited BCR paths: **two**
explicit Dirt materials qualify, while BeachGrass, forest, grass, seabed, pebbles,
and other unknown/hard identities remain dry. The flat native candidate is
`9481.25 2994.25 209.72 0 -18`, material 1,
`Terrains/Common/Surfaces/Dirt_02.emat`. It is an open patch with two boulders,
not a building interior. The earlier BeachGrass candidate is rejected evidence,
not a puddle acceptance site.

- `dirt-wet-20260930-102108` versus `dirt-dry-20260930-102243`: wet override
  creates connected blue-grey sky-reflecting patches; dry control has no film.
  Both retain ground detail. Objects cast shadows but are **not** reflected.
  Both runs have an idle object preparer at the final cut. Terrain colour is
  1.110 versus 1.003 ms in single snapshots, not a repeated median benchmark.
- `dirt-aerial-20260930-103219`: same area from
  `9481.25 3006.25 244.72 0 -86`; broad dark patches retain the ground boundary,
  adjacent grass stays dry, and there are no bright fine sun points in inspected
  stills. Scheduled capture sequence is not proof against every-frame glint.
- `dirt-snow-20260930-103316`: `POSEIDON_SNOW_TEST_DEPTH=0.2`, wet override still
  one. The covered ground is white and the puddle film disappears. The isolated
  process setting does not change the owner's snow profile.
- `dirt-master-off-20260930-103612`: wet override one with master enabled zero;
  uploaded wetness is zero and the puddle layer is disabled for the process.
- `dirt-weather-varying-20260930-102907-041e0e`: no forced rain or wetness
  override. The harness uses real weather setters and acceleration 3 to bound
  the lifecycle test. Captured rain 0.702 fills accumulated/uploaded wetness to
  0.6833. Pause reports mission time 55.114 twice five wall-clock seconds apart;
  diagnostic generation and last uploaded/accumulated samples remain identical.
  After rain stops, actual density is zero and wetness falls to 0.5819 at
  presentation simulation time 90.780. Normal exit, stamp and binary hashes
  are retained. This proves fill/partial drying and clock freeze, not complete
  drying, save/network state, rain collision or all-map roof coverage.

The initial lifecycle attempt correctly rejected its own constant-full-rain
assumption: actual weather had already decreased to 0.743 despite the immediate
setter, while wetness reached 0.7007. The corrected runner admits varying real
rain and retains its actual telemetry; no fixed-rain override was added to make
the test pass.

### Rain-cover correction

`0d3cd4d5` corrects a source-policy counterexample: five-direction diffuse sky
reach can be about 0.72 beneath a small canopy with open sides, enough to admit
some puddle film. Puddles now query the existing vertical/zenith depth layer,
independently of tilted windows and artistic ambient controls. Two focused
tests compose the production helper with real frame bindings and execute it
on a GPU with synthetic directional depth maps. Covered zenith/open sides
rejects film, open zenith/blocked sides admits it, and zero ambient strength
or a high ambient floor cannot invent rain. These tests require an adapter;
absence is a failure, not a skipped acceptance result.

No-map, depth-outside and feathered-map-border open fallbacks remain explicit.
This is a bounded cosmetic rain-cover query, not terrain-wide rain collision.
Actual native roof-over-dirt acceptance remains separate from this directional
GPU falsifier. Oliver has deferred his appearance tuning and asked us to use
our own judgement and continue building.

### Repeated settled timing

`build/grass-puddle-perf/Puddles-20260930-114753/results.json` records three
alternating dry/wet pairs on the same clean installed `a001b55f` pair, with
40-second warmup, one terminal screenshot per run, native 1280x720, MSAA 4,
DLSS off, the validated Dirt camera, fixed hour 16 and actual rain zero.
All six runs exit normally, the native preparer is idle at the cut, and installed
hashes/stamp remain unchanged. No concurrent game, GPU tests or heavy builds
ran during this campaign. Per-run raw metrics and provenance are retained.

Median final terrain-colour GPU snapshots are **0.991 ms dry / 1.093 ms wet**
(+0.101 ms). The three wet-minus-dry snapshots are +0.025, -0.011 and +0.119 ms,
so this is a small observed cost with timing noise rather than a universal
overhead bound. Median CPU-ring frame p95 is **23.876 / 23.691 ms**; main-object
triangle counts differ substantially between arms despite identical 22,113
registered instances. Do not claim an FPS improvement or whole-frame causal
equivalence from those values. The later zenith correction is not in this
installed pair; its reduced query count still needs installed verification.

The [controlled stability record](MUD-PUDDLE-STABILITY-20260930.md) retains
failed and passing camera-return campaigns. Nonempty Dirt/overhead masks return
with IoU 1.0, and the final ground-only campaign passes unchanged numeric gates.
The [stock floor-free tent record](MUD-PUDDLE-STOCK-ROOF-FIXTURE-20260930.md)
accepts actual retained roof exclusion on installed `77299a74`: visible wet/dry
response disappears with the tent and returns after removal, while exposed dirt
remains wet. This is bounded fixture acceptance, not full-world rain collision.

The source-confirmed BeachGrass cell also passes with zero wet/dry changed
pixels. Original Everon now accepts its verified bare cultivated-soil surface
through exact cached metadata, with all four quadrants and no clutter character.
Installed `8b3ae13c` passes soil response, camera returns, master-off and pure
Grass exclusion; [the CWA record](MUD-PUDDLE-CWA-CULTIVATED-SOIL-20260930.md)
retains the source census and bounds. No broader Field-name admission was added.

Pending: later owner tuning and wider map/material-generation coverage.
Do not mark the broader wet-surface/hydrology roadmap complete from this slice.
