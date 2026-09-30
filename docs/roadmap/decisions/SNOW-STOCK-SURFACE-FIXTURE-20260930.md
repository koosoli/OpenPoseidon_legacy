# Installed stock snow surface fixture

`scripts/Test-SnowSurfaceCoverage.ps1` supplies a reproducible capture campaign
for the shared snow material and object-cover implementation. The script does
not accept snow appearance from image differences. Its final successful status
is `captured-not-visually-accepted`; installed appearance acceptance belongs to
the recorded surface controls and source admission evidence.

## Fixture and views

The stock `CampEastC` / `stan_eastC.p3d` tent has an audited floor-free central
region. See [the stock roof audit](MUD-PUDDLE-STOCK-ROOF-FIXTURE-20260930.md) for
asset hashes, geometry and the independently accepted native dirt rain-cover
fixture. The snow runner reuses the accepted centre 9486/3006, exposed centre
9475.25/3018.25 and the actual terrain-height/class/model/ASL checks in
`TerrainPuddleStockRoof.ps1`. It never modifies installed models or PBOs.

If queried centre ground height is `g`, model origin is
`g - (-2.8100528717041) + 0.02`; the tent's maximum source height is approximately
`g + 5.64011`. Views are candidates until their actual visible surfaces have
been inspected; no pixel segmentation follows merely from camera coordinates.

| View | Camera X/Z/height/azimuth/elevation | Required interpretation |
|---|---|---|
| roofTop | 9486 / 2998 / g+12 / 0 / -45 | Exposed upward roof fabric is the object-coat positive control. |
| roofFar | 9486 / 2974 / g+28 / 0 / -35 | Same roof at distance; resolved powder should converge without glitter. |
| underside | 9486 / 3004.5 / g+1 / 0 / 70 | Floor-free central interior looks upward; ceiling fabric stays bare. |
| wall | 9494 / 3006 / g+1.8 / 270 / 0 | Tent vertical wall faces stay bare; ignore roof pixels in a wall ROI. |
| coveredGround | helper's own queried ground+1 / 0 / -55 | Same terrain beneath the retained roof; source/shelter admission controls. |
| exposedGround | helper's independent queried ground+1 / 0 / -55 | Open terrain must visibly respond to deposit. |
| terrainGrain | exposed ground+0.4 / 0 / -55 | Near powder/soil grain and actual stock infantry depression. |
| roadNear | 5083.67 / 3988.25 / 17.63 / 145.4 / -18.5 | Existing `Test-SnowRoad.ps1` stock Eden road pose. |
| roadFar | 5020 / 4080 / 55 / 145.4 / -18.5 | Existing stock Eden distant road pose. |
| vehicle | actual UAZ ASL +4 X, -5 Z, +4 height / 321 / -27 | Exploratory ordinary-object view; dynamic/animated mesh admission may exclude it. |

`-LegacyCwa` removes native-world environment overrides and uses classic Eden.
This establishes the map provenance of the existing stock road cameras. In the
default native-world campaign, those two road poses are supplementary: no native
road at those coordinates has yet been established. The helper still queries
actual heights in either world; a legacy tent placement is not automatically an
accepted terrain-shelter fixture just because native placement passed.

## Serial campaign and actual-state gates

Three fresh, locked installed processes have private profiles:

1. Dry / object snow requested ON.
2. Actual deposited depth / object snow ON.
3. The same actual deposited depth / object snow OFF.

Every arm captures disabled initial, enabled, moved/returned, disabled after and
restored views. The view sweep travels to the road and returns to the exact roof
and ground poses, while simulation is paused at 100 seconds. It captures an
actual stock UAZ, then removes it before a normal exit.

The deposited arms create and move a real stock `SoldierWB` prone unit along the
exposed ground. The runner requires actual movement, increased stored chunks and
a positive `dev_snow sample` in the bounded trailing body footprint. Disabling
and reenabling snow must retain the exact chunk count and positive sampled
deficit. A final settings reset must disable cover, restore documented settings
and retain the actual depth and chunks. The script captures that disabled reset;
it does not confuse the reset's stored `falling=true` with active precipitation.

`POSEIDON_SNOWLINE=off` is set before launch. `POSEIDON_SNOW_TEST_DEPTH=0` creates
zero-depth test state with `falling=false`. The rate variable is removed: its
mere presence changes `falling` to true, even when its numeric value is zero.
Every material capture checks actual `dev_snow` enabled/falling/geometry/depth
and bounded integral chunk state. Actual rain/fog must remain zero. Unknown
harness actions or evaluator values fail honestly.

The opt-in `WGR_SNOW_SURFACE_FIXTURE=1` producer telemetry must identify effective
object settings at the enabled capture stage:

```text
wgpu object snow: enabled=1 deposit=0.1800 snowlineHeight=-1.00 snowlineRange=0.00 snowlineDepth=0.0000
```

ON must upload the requested actual depth; OFF must upload zero object deposit
despite retained terrain snow. Both must upload a disabled effective snowline.
The runner refuses missing or incompatible telemetry; assignment of an
environment variable alone cannot establish an effective feature toggle.

Temporal AA, auto exposure, grass, wind, clouds and terrain UV jitter are pinned
for diagnostic captures. Puddles are disabled. This isolates the snow material;
it does not replace a later ordinary grass/temporal appearance check. The
`-PowderOff` switch sets `WGR_SNOW_POWDER=0` for a separate identical three-arm
material ablation. Powder defaults explicitly to ON in every regular run.

The script records deployment stamp first, matching before/after binary hashes,
file sizes/timestamps, script/helper/metric hashes, source head, actual positions,
camera poses, actual snow state, effective object telemetry and harness traffic.
PNG signatures, owned normal exits and shutdown markers are mandatory. Only its
own process may be killed after failed graceful cleanup; that fallback is recorded.
Process environment is restored at completion. Artifacts live in ignored
`build/snow-surface/<label>-<time>-<id>/`.

## Appearance/source acceptance after capture

- Select actual roof-fabric pixels common to dry, deposited ON and deposited OFF
  at the same pose. ON must visibly coat the exposed upper roof while OFF
  retains the dry material. A nonempty terrain delta elsewhere in the frame is
  insufficient. Require actual retained tent/source admission metadata.
- Select actual ceiling and wall pixels separately. Their object ON/OFF response
  must stay bare; nearby snow ground, roof and lighting can change whole-frame
  statistics. No hard visual tolerance is invented by the runner.
- Inspect covered and exposed ground at the verified visible soil/rock ROI,
  ensuring ground is not hidden by a tent floor. Shelter is independent from a
  dark room. Tracks must survive camera travel and disabled/reset intervals.
- Inspect classic Eden road near/far controls. Existing road behavior is to hide
  fully ground-conformed road/decal fragments under deposited snow of at least
  four centimetres, sharing terrain deficit/shelter depth. Rigid bridges remain
  separate; altitude-only mountain roads remain visible asphalt. Snowline is
  deliberately off here, so a separate altitude-only run is needed for that rule.
- Use near/far/return views plus powder ON/OFF to judge whether world-anchored
  resolved grain filters toward its mean without high-frequency sparkle.
  Central-60% RGB metrics are descriptive, not semantic snow or quality masks.
- UAZ is exploratory until source telemetry confirms its owner mesh is eligible.
  A dynamic/skinned/CPU-posed/animated object exclusion is a supported boundary,
  not automatically a snow defect. This fixture does not claim all rocks, crates,
  canopies, models, maps, animation paths or off-map fallback coverage.

## Commands and checks

Root remains the sole builder, deployer and installed-game operator. From Git
Bash in its integrated checkout, after the matching build/deploy:

```bash
LOCK_OWNER="snow surface native" scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Test-SnowSurfaceCoverage.ps1 -Label native-default
LOCK_OWNER="snow surface Eden" scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Test-SnowSurfaceCoverage.ps1 -LegacyCwa -Label eden-default
LOCK_OWNER="snow powder Eden ablation" scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Test-SnowSurfaceCoverage.ps1 -LegacyCwa -PowderOff -Label eden-powder-off
```

Pure checks, with no game/TCP/GPU:

```powershell
./scripts/Test-SnowSurfaceCoverageHelpers.ps1
./scripts/Test-TerrainPuddleStockRoof.ps1
```

The first extracts the runner's actual helper definitions through the PowerShell
AST and checks invariant camera formatting, nonfinite rejection, SQF literal
backslashes/doubled quotes, ASL arrays, actual state/refusal gates, disabled reset
state and ON/OFF uploaded parameter validation. The existing roof helper mock
suite checks actual queried heights, class/model/placement and cleanup refusals.

Existing native tests cover persistent SnowField storage/presses/disable/reset,
snow shelter columns above/below roof height, and snowflake projected area.
Renderer `road_and_terrain_share_snow_depth_evaluation` pins the shared depth
function and deposited-road discard. Root's actual GPU material test
`snow_powder_gpu_filters_distance_and_obeys_surface_admission` probes finite
albedo/unit normals, zero dry/wall/underside/shelter coverage, partial/full cover,
and distant mean filtering; the fragment-storage layout device test checks the
shared conform binding budget. These are substantive prerequisites, but none
alone proves actual stock roof pixels or camera-consistent object admission.

This commit adds scripts and fixture documentation only. No runtime build,
installed launch, GPU test, deployment or appearance acceptance was performed by
this subagent.
