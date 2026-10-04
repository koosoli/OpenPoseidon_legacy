# Original OFP distant terrain coverage, 2026-10-04

## Report and source cause

Oliver's original Everon aerial screenshot (`Screenshot 2026-10-04 104300.png`)
shows green land ending in a pale blue coastline silhouette. Camera coordinates
were not supplied; the suggested route below is a reproducible similar view,
not a reconstruction of that exact screenshot.

`Landscape::CalculBoundingRect` derives a land rectangle from the scene's
display fog distance. `TerrainWgpu::DrawTerrain` formerly rejected every CDLOD
node outside that rectangle and every node behind the CPU camera's far plane.
The wgpu projection is infinite-far (`EngineWgpu::PushSceneCamera`), and water
already selects the whole existing heightfield tree without the CPU far plane.
With weather fog zero, opaque ground can disappear while water's heightfield
coverage still recognizes the dry coast, exposing sky rather than ground. Node
rejection is discrete, so crossing the distance boundary exposes whole patches.
Original OFP ground textures are eagerly uploaded; the modern camera material
paging path is not the cause of this particular original-map coverage mismatch.

## Change and limits

The default wgpu terrain selection covers its existing map-centred heightfield
tree, retaining lateral and near frustum culling, height bounds, the existing
distance-driven LOD ladder, morph bands and contact refinement. It ignores the
CPU far plane to supply distant opaque background, matching the GPU projection.
One selection partitions the tree: the near region is not drawn twice. The
map-only extent still clips to the authored map; the normal extended extent
keeps its existing clamped-edge seabed policy. No heights, textures, material
readiness, fog, weather, object visibility or renderer ABI are changed.

`WGR_TERRAIN_BACKGROUND=0` is a startup-only old rectangle/far-plane control.
The default is `1`. This does not make distant objects resident or resolve
coarse silhouette error at arbitrarily large distances. Additional background
patches have a CPU/GPU cost which requires installed measurement.

The read-only `terrain_coverage` harness command publishes the latest producer
selection: world, frame, exact current heightfield revision/range/spacing,
camera, old rectangle, existing tree bounds, patch and LOD counts, and the number
of patches wholly outside the old rectangle. `farthestPatchXZ` is horizontal
corner reach, not a 3D clip distance. The getter rejects a replaced landscape;
the handler rejects changed source/revision and non-main-thread access. It is
a producer selection witness, not GPU completion, texture completion, or pixel
proof. An unavailable/empty selection is an error, never a fabricated zero.

## Native source checks

Private actual Catch2/clang-cl CPU tests use the production `BuildCdlodTree`,
`CdlodHeightBounds`, `SelectCdlod` and shared rectangle predicate at the original
island's 2048 samples / 6.25 m spacing. Heights are deterministic fixture data.
At 5, 1200 and 5000 m camera heights they prove:

- Every old rectangle-selected near patch retains exact origin, size, level and
  morph endpoints in the background selection.
- The existing tree is covered exactly once, including distant island and
  extended seabed points, with no overlapping parent/child draws.
- The normal LOD ladder remains below 1024 selected fixture patches.
- Empty, reversed, nonfinite and invalid-size rectangle inputs are refused.

Result: **138,602 assertions / 2 cases PASS**, registered in `PoseidonTests` as
`[terrain-background]`. Both changed production translation units
(`TerrainWgpu.cpp`, `HarnessBuiltins.cpp`) passed current-source syntax checks.
Full root build and installed appearance/cost acceptance are still pending.

## Installed root-owner test route

Use the normal game lock and matching installed executable/DLL pair. Launch
untouched original Everon with `tests/perf/missions/perf_field.eden`, clear any
Reforger/world override, day 12:00, dry weather, zero fog. Keep exposure, render
resolution, temporal mode and all other settings identical in both arms.
Run two fresh processes with `WGR_TERRAIN_BACKGROUND=0` and `1`.

After scene readiness, set `triSetViewDistance 3500`, weather
`0 setRain 0; 0 setOvercast 0; 0 setFog 0`, then pause only simulation after
weather/ranges settle. Set these raw engine `[X,up,Z,dx,dy,dz]` camera requests:

1. `triSetView [6000,1200,4000,0,-0.1816,0.9834]` (north-facing upper island).
2. `triSetView [6000,1200,4500,0,-0.1816,0.9834]` (bounded motion across old cutoff).
3. `triSetView [6000,5000,4000,0,-0.65,0.76]` (high, steep aerial control).

Wait actual rendered progress, query `terrain_coverage`, `weather_visibility`
and `frame_performance` before/after each screenshot. Require original
`eden.wrp`, unchanged native revision/spacing, eager materials, matching camera
and weather/ranges. Background ON should select additional distant patches;
OFF should retain the legacy cutoff. Inspect the entire opaque coast and land
silhouette, not merely a positive patch count. Then test VD 50000 at the same
views, a near coastal view, nonzero fog, and the map-only extent control.

Cost: use an uncontended matched OFF/ON comparison with at least 256 rendered
warmup frames per view and fresh asynchronous `frame_performance` samples.
Compare whole-frame GPU/CPU and land phase counts; do not sum inclusive timers
or claim an exact screenshot-frame GPU sample. Root owns actual deployment,
runtime, picture judgement and acceptance ledger.
