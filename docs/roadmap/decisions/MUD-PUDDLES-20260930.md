# Rain puddles on mud — bounded first slice, 2026-09-30

Oliver explicitly prioritised mud puddles and authorised parallel implementation from
`codex/streaming-residency` (`ed452be8`). The roadmap's earlier wet-surface ticket excludes
puddles; this owner request authorises a focused extension rather than a broad hydrology
or terrain-accumulation framework. The integrating root owns full build/deployment/runtime
acceptance. This decision records source implementation and CPU/shader checks only.

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
  the film. Raw geometric interior sky reach suppresses darkening and reflection under
  shelter, independently of artistic ambient strength/floor sliders; existing ambient
  shading still uses those controls. The extra raw map query runs only at wet eligible
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

Pending: matched full game build, installed startup/smoke, appearance comparison, timing,
and Oliver's visual preference. Do not mark the broad wet-surface or puddle roadmap work
accepted from this source-only slice.
