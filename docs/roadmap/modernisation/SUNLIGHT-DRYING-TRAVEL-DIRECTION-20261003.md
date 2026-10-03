# Actual sunlight drying direction, 2026-10-03

Source candidate: physical/cosmetic solar call sites and a shared pure helper.
No evaporation, infiltration, rainfall, wind or flow coefficient is changed;
no water is deleted or seeded in an installed mission. Root owns matched build,
deployment, device checks and actual sunny runtime verification.

## Demonstrated producer contract bug

`LightSun::SunDirection()` returns the astronomical **light-travel** vector.
`Lights.cpp` stores `_sunDirection = _direction` before the night moonlight
swap, documents the travel direction and tests daylight with
`float sinSun = -_direction.Y()`. The real ephemeris likewise sets
`_direction = -sunToDirection`. A daylight sun above the horizon therefore
has a negative travel Y. `PresentationSnapshot` copies this same value.

Both drying consumers incorrectly clamped the positive travel Y:
`World::SimulateCloudlets` for retained water, and `TerrainWgpu` for cosmetic
ground wetness. Daylight therefore supplied zero solar drying. Night's
positive travel Y was suppressed by NightEffect=1. The sun-dependent
coefficient existed, but ordinary sunshine did not reach it.

The shared resource-free `SunlightDryingExposure` now evaluates:

```text
clamp(-sunTravelY, 0, 1)
  * (1 - clamp(actualOvercast, 0, 1))²
  * (1 - clamp(actualNightEffect, 0, 1))
```

Nonfinite inputs conservatively return zero. Both consumers still use the
actual astronomical sun, actual overcast and actual night effect. Neither
uses the moon-swapped shading direction, camera angle or a forced sun value.
The wind paths remain actual speed/20 with existing downstream clamping.

## Separate histories and unchanged time scales

`GMud::Wetness` is a gameplay soft-soil history with a rain-free e-fold of180
simulation seconds. It is not the terrain shader's accumulated wet amount.
That shader receives the separate `TerrainPuddles::Wetness` history, whose
rain-free e-fold is `180 / (1 + 2*solar + 0.5*wind)` simulation seconds. At the
theoretical solar1/wind0 bound this is60s, versus180s with no solar. Starting
from wetness1, the cosmetic film's0.18 eligibility cutoff is reached after
about103s or309s respectively. Paused/duplicate terrain draws remain inert.
The earlier wet-soil note is corrected to identify its GMud-based numeric
example as conditional, not measured terrain-upload telemetry.

Actual retained-water depth uses a different physical budget:

| Component | Existing rate in metres per simulation second |
| --- | --- |
| Rainfall | 0.00012 × actual liquid rain |
| Infiltration | 0.000004, limited by available water |
| Evaporation | 0.000002 + 0.000006 × solar + 0.000001 × wind, limited by available water |
| Flow/outlet | Actual head differences, bounded donor volume and speed; sea is an outlet |

For a closed flat pool without inflow, total depth loss is6µm/s in still
shade and12µm/s at the theoretical solar1/wind0 bound. In600 simulation
seconds that removes3.6mm or7.2mm. A10mm closed pool would require about28min
or14min; a100mm pool about4.6h or2.3h. Incoming catchment flow can keep a
local hollow wet while global volume falls. These are the engine's existing
coefficients, not measured material infiltration or validated meteorology.

`UniformWettingDensity` uses the maximum of actual weather rain and visible
liquid-particle override. Turning off weather rain alone cannot stop retained
input if a liquid-particle diagnostic is still active. Snowflakes alone are
excluded. Cosmetic drying likewise remains paused with the simulation clock.
An opt-in fine field with stale/unproven source defers physics until source
repair; it retains mass rather than inventing dry cells. Normal baseline OFF
does not have that fine-source deferral.

## Actual baseline local drainage

Root completed installed b59f455a campaign
`build/rain-water-natural/actual-local-sunny-drain-20261003-121251-626627`
with stable pair and normal exit, CPU/renderer fine OFF, natural Noe rainfall,
wind override0, terrain cosmetic puddles disabled, then requested sunny noon
and no rain. The installed baseline still contained the wrong solar sign.
Actual sunny exposure was not separately queried, and these results cannot
be attributed to the sun-dependent coefficient.

At the same native point(4975,4675), depth was4.516731mm after rain,
4.252554mm at the no-input baseline, and exactly0 after604.316 simulation
seconds. Native terrain height remained21.55499839782715m. This establishes
local dry-out at that thin point, without forcing water or changing terrain.
Root's low/top pictures have no readable actual pool at this4mm location;
they do not establish visible pool disappearance.

No-input global volume fell531384.9921223047→319435.5768117432m³.
RainVolume remained exactly786276.7127288096m³; actual stopped/drained
weather rain, particle density and liquid rain were all0. Budget deltas:
infiltration34798.391550487184m³,
evaporation17074.39768271732m³,
sea outlet160073.07415015093m³.
The3.551927206106484m³ residual is the coarse float accounting difference
within the existing campaign gate, not an exact mass-equality claim.
The dominant global loss is outlet flow. Local flow is nonzero at stop, so
the local disappearance is not an isolated evaporation-rate experiment.

## Validation and next actual gates

`scripts/Test-SunlightDrying.ps1` passes with clang++ C++20/Werror. It verifies
the actual sun-travel producer, snapshot copy and both actual helper call
sites, then exercises the production helper, TerrainPuddles wetness and both
coarse/fine RainWater implementations. Day/night/overcast/invalid inputs,
unchanged rates, increased actual wind, budgets and pause are checked.
No game, device or full build is used by this test. Existing physical water
tests remain applicable.

Root should run matched sunny and cloud/shade cases with actual sun travelY,
overcast, night effect, exposure and liquid input recorded, comparing the
same local depths and budget deltas. A readable native shallow pool is needed
for visible drying acceptance; a global volume decrease or a thin point
already visually dry does not suffice. Preserve actual deep-pool retention,
source/owner/mass guards and normal exit/provenance evidence.
