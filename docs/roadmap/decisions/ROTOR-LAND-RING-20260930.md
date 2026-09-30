# Rotor land ring

## Scope and source authority

The owner requested dust/sand/snow thrown outward under hovering helicopters.
Existing water downwash is accepted and explicitly excluded from this change.
No water renderer, interaction or ripple code is changed. Existing snow-field
rotor erosion is preserved without a change to its rate or footprint.

`Helicopter::Simulate` supplies actual rotor RPM, upright orientation, position,
simulation delta and visual importance. `AirflowField::EvaluateRotorWash` supplies
the established outward ground jet. A new short-lived `RotorLandPuff` uses the
existing cloudlet billboard, fading, lighting, dispersal and room containment;
it additionally samples live `GAirflow` every simulation tick. Particles stay
in world coordinates and never follow the camera.

Eight changing, deterministic annulus samples are considered at 4 Hz. Each
sample independently requires actual dry terrain above sea level, no raised
roadway, actual authored `SurfaceInfo::_dustness` or remaining deposited snow,
and a clear physical VIEW-geometry ray from ground to the aircraft, ignoring
only that aircraft. Wet soil and actual liquid rain suppress dust. A valid
`GRainWater().At` depth exceeding 3 mm suppresses this land effect, including
powder; no fallback water disturbance is introduced. An unconfigured hydrology
field uses the existing sea/rain/soil-wetness controls.

Snow amount is actual base cover minus the stored local erosion deficit,
sampled from the existing snow field. This is a visible powder cloud, not a new
snow simulation or a replacement for the existing persistent erosion.

## Bounds and controls

- Rotor RPM must exceed .2, hover height must be 0–30 m and local up at least .5.
- Camera distance above 350 m, dead aircraft, disabled local airflow and zero
  simulation delta emit nothing.
- Maximum 128 live rotor puffs across the world; maximum 32 support/roof probes
  per quarter simulation second across all helicopters, including sheltered
  aircraft that emit no particles. All particle ownership belongs to the
  existing world cloudlet list and mission teardown.
- Particle lifetime is approximately 1.45 simulation seconds, with fade-in,
  finite outward velocity and live airflow response. Particles are excluded
  from the smoke shadow/froxel substitution so they remain dust/powder sprites.
- `POSEIDON_ROTOR_LAND=0` disables only this addition. Optional
  `POSEIDON_ROTOR_LAND_TRACE=1` logs `ROTOR_LAND` actual source position/RPM,
  sampled height, emitted/snow/sheltered counts, current live count and liquid
  rain every eight emission batches. Existing water behavior remains unchanged.

## Validation and acceptance

Focused C++20 warnings-as-errors tests execute the production rotor policy,
annulus and global probe budget. They test RPM/height/upright attenuation,
sea/road/roof/hard/wet/rain exclusions, actual production RainWaterField samples,
snow admission, malformed values, deterministic finite annulus support and
probe-cap/reset behavior. Source call-site contracts require actual terrain,
hydrology, physical ray, airflow and preserved snow-erosion integration.

`scripts/Test-RotorGroundPolicy.ps1` passed with LLVM clang++ on the agent
worktree. This does not compile the whole Helicopter translation unit and does
not establish installed appearance, frame cost or correct stock aircraft
fixture geometry. Root owns the full build and installed hover test.

Required installed controls: rotor off versus stable dry-sand/dust hover;
identical hover over actual deposited snow; 30 m+ altitude; dry hard roadway;
real roof between rotor and terrain; rain/standing-water dust suppression;
pause and camera move/return; multiple helicopters/global bounds; ocean
downwash remains visually identical. Source traces and captures must identify
actual stock model/pose and precipitation. No synthetic terrain stamps are
used, and no installed acceptance is claimed here.

Remaining limits: source-authoritative dustness can differ across mod material
palettes; no colour/name inference licenses unidentified sand. The first
visible effect is a diffuse annulus of dust/powder puffs, not individual grains.
Existing cloudlet room containment does not promise per-grain collision with
every outdoor obstacle. Probe budgeting is deterministic first-come within a
simulation bucket; very large close fleets may share fewer than eight spokes.
