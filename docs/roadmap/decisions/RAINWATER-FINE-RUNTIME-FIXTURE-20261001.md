# Native edited-hollow runtime fixture

Source candidate; installed execution, complete engine compilation, GPU tests
and visual acceptance remain pending. This fixture is based on b7debd26 plus
the atomic ABI-19 handoff 35ad3083. It does not turn the default-off fine
simulation or renderer on in ordinary missions.

`scripts/Test-RainWaterFineRuntime.ps1` launches two serial, private original
Nogova missions under the existing game lock. Each uses the installed EXE/DLL,
records DEPLOYED-FROM and hashes before/after, owns only its process, and restores
the genuine terrain-edit baseline before normal exit. Failure retains RPCs,
logs, screenshots and the source line/stack. Forced cleanup is an explicit
failure. No installed assets, water quantities, simulation heads or geometry
are manufactured by the runner.

## Genuine edit and bounded diagnostics

`dev_terrain_brush` is registered beside `dev_physics_probe`; it requires `--dev`,
a single-player world and an actual finite supported landscape. `paint` permits
finite in-domain coordinates, radius >0 and <=100 m (including the existing
minimum effective radius) and nonzero delta of at most 8 m. It calls the
existing `PaintTerrain` transaction: real Box3D registration/patch must succeed
before native vertices are changed through `HeightChange`. `restore` calls
`RestoreEditedTerrain`; it does not bypass collision or the edit journal.
`state` reads the actual nearest vertex, native grid/range/revision, baseline and
registered collision dimensions. Its same-command `fineSourceReady` receipt
lets restore prove immediate stale-source closure before a later World tick
refreshes that revised terrain. Responses use `HarnessProtocol::JsonResponse`
including the TCP newline; they do not change the separate weapon-state getter.

The old read-only `dev_rain_water state` adds feature/source/tile status.
`fine_state` reads the actual <=8*1024-cell snapshot, current source identity,
revision/cardinality and integrated volume. Optional finite x/z selects one
owner's rectangle, head, volume, parent and four actual source heights ordered
00, X1, Z1, 11. Local support is computed using the same production
`RainWaterCellGeometry::Height`; integrated volume uses its actual triangle
storage curve. No query advances the simulation. World pointer identity is
returned as a decimal string to avoid JSON's 53-bit integer rounding.

## Gates and controls

- Default OFF removes both fine environment switches; ON sets
  `POSEIDON_RAIN_WATER_FINE=1` and `WGR_RAIN_WATER_FINE=1` in a fresh process.
  Both use `setTerrainGrid 6.25` and verify the actual resulting source spacing.
- Default centre 5012.5/4662.5 is a real native vertex halfway between old 25 m
  nodes. The brush lowers it by 1 m over radius 18 m. The fixture verifies the
  committed height and collision registration, then requires eight surrounding
  native rim samples above the lowered centre. This is a bounded source fixture
  suitability check, not proof that every intervening world saddle is closed.
  An unsuitable real stock slope fails honestly; X/Z may select another actual
  native midpoint. No synthetic basin is substituted.
- A dry edit queues its actual footprint while the entirely dry rainwater field
  remains unconfigured. No water or fine owner is expected until the first real
  liquid input. Actual `0 setOvercast 1; 0 setRain 1` and `weather_visibility`
  establish natural rain; visible-particle override and artificial wetness are
  cleared. The test waits real simulation time at acceleration 4.
- The default-off field must remain coarse. ON must have source-ready valid
  native geometry, 1..8 complete tiles, 1024 cells per tile, exact current
  HeightRevision and field generation/revision, and triangle-integrated fine
  volume matching held owner volume. Global rain-minus-losses must equal water
  volume. The crater point must actually be wet.
- Five subcell queries require one identical horizontal head over the actual
  sloped triangles, including both a wet and a dry point; depth is clipped to
  actual support. This differentiates real fine geometry from a sloping depth
  proxy or a rectangular all-wet overlay. A fully wet/unsuitable selected cell
  fails the clipping fixture rather than weakening its proof.
- Pause, camera-away/return and read-only queries must preserve clock, water
  revision/generation/volume. Matching paired renderer evidence is mandatory
  for ON. Exact grammar is `Rain water publication: worldToken=... generation=...
  heightRevision=... terrainRange=... terrainSpacing=... revision=... flags=...
  copiedCoarse=... copiedFine=... cachedCoarse=... cachedFine=... tiles=...
  enabled=... accepted=...`. Token, revisions, native source, flags 7 and accepted
  enabled pair must match the paused CPU snapshot. Coarse counts are Vec4 **cell
  counts**, width*height; fine counts are actual FineCell records. Parameter-only
  updates may copy zero records only while the complete matching cached pair
  remains. Unknown grammar, stale receipt, rejection and partial arrays fail.
- A genuine stock CampEastC roof is placed over the paused hollow, captured and
  removed. CPU quantities must remain unchanged. CPU rain is whole-owner and
  does **not** exclude roofs; this capture awaits actual renderer physical-cover
  GPU/visual verification, and is not an asserted CPU shelter negative.
- The actual corner sea bed and zero standing-water depth form a bounded outlet
  negative. Maps without that corner-sea fixture fail rather than assuming sea.
- Real weather leaves storm conditions and stops rain. With no new rain volume,
  water must drain gradually, retaining some stored volume. Restore recovers
  native source heights, immediately invalidates stale fine source and then
  refreshes current geometry without inventing water.

## Validation and remaining evidence

`pwsh -NoProfile -File scripts/Test-RainWaterFineRuntime.ps1 -SelfTest` checks
exact accepted receipt versus stale identity/revision, rejection, incomplete
arrays, float-lane/cell-count confusion and unknown output; it also rejects
source gaps, inconsistent triangle mass and global budget, and preserves SQF
raw-backslash strings. Existing `Test-RainWaterField.ps1` and
`Test-RainWaterFine.ps1` pass against the production pure cores/geometry.
These are CPU/source checks, not installed renderer acceptance.

Root must compile the actual HarnessBuiltins translation unit and matched
ABI-19 C++/Rust consumer, execute paired publication/triangle clipping/roof
mask/stale-source GPU tests, deploy together, then run under its game lock:

```powershell
pwsh -NoProfile -File scripts/Test-RainWaterFineRuntime.ps1 -Label native-crater
```

`-OnlyOn` narrows troubleshooting but does not replace the OFF comparison.
Before accepting appearance, inspect near/top/return photos for horizontal
water bounded by true terrain, absence of square/coarse duplicate patches,
physical roof suppression and no sea overlay. Also measure native edit/source
upload and steady wet fine CPU/GPU cost. This bounded opt-in fixture does not
establish all-map micro-hollows, wet-tile eviction policy, complete shelter
hydrology, swimming/buoyancy or a full-world 6.25 m water grid.
