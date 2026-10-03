# Aircraft rain population and water downwash audit

Source baseline: `8aca5c43`, matching installed EXE/DLL stamp verified before
inspection. Root owns full build, GPU verification, installation and actual
flight/roof/ground smoke tests. This branch has only source and CPU validation.

## Liquid rain population

`RainSystem::Spawn` used the nominal camera spawn height / fall speed to compute
birth rate: stock 1800 target, 18 m height and 9 m/s yield about 900 births/s at
full density. Liquid drops previously had no camera-volume retirement and no
density-scaled target-count cap. They survived until ground/roof collision.
From an aircraft, the ground lies far below the camera: the 4000 hard maximum
therefore fills with retained particles outside the local precipitation volume.
Every retained particle continues room/shelter probes and ground queries even
when no longer visible. Snow already retires camera-exited particles.

The liquid candidate retires particles outside the camera-centred radius and
spawn-height cylinder **before** birth/occlusion simulation, and limits the
population to `ceil(min(targetDrops,maxDrops)*clamp(density,0,1))`. Retiring a
visual does not create a splash, terrain hit, roof kill or water impulse. Room,
forward-path, spawn shelter and actual ground/roadway rules for retained drops
remain unchanged. Birth rate and private random stream remain unchanged; full
targets and falling density do not accrue spawn debt. This does not modify
rainfall input to hydrology, material wetness or uniforms.

`POSEIDON_RAIN_TRACE=1` enables ordinary mission rain's existing per-phase timing
trail without the forced-weather test hook. It appends volume retirement count,
population retirement count and current population limit. Other values do not
enable the trace. The trace is observational.

`scripts/Test-RainParticlePolicy.ps1` compiles the production pure policy with
Clang C++20 / warnings as errors. Tests cover camera/window movement, exact
boundaries, invalid values, density monotonicity and configured target/max caps.
A deterministic 30-second birth/fall exercise keeps eye-height ground impacts
and particle-update work identical. At 117.58 m its former population exceeds
3000; the new population stays at most 1800, independently of camera altitude
(also exercised at 1000 m). Synthetic particle-update work is 6,668,665 ->
3,132,900. This is **not** an actual FPS, frame-time or shelter-query benchmark.
Actual stationary/flying rain appearance, source timings and roof exclusion
remain required on the installed combined build.

## Water downwash backend finding

The current OP water producer is map-neutral. PresentationSnapshot includes the
occupied helicopter plus the nearest spinning rotor sources, with native ground
height. `WaterWgpu` admits sea-relative altitude [-2,30] m and rejects ground
above sea+0.25 m. Its 256 m interaction domain follows the camera, so distant
helicopters outside that patch cannot affect their visible far water.

Tidewater `impacts::submit` deliberately ignores zero-ammo-mass events, including
the current OP rotor impulse. Tidewater's wake implementation also documents
that OP's interaction field is not drawn in that backend. Therefore rotor wash
can work in current OP and be absent in Tidewater without a map-specific filter.
This is a source-proven backend integration gap, not yet a reproduced Nogova
runtime diagnosis. The exact active backend, occupied camera, sea-relative
altitude and admitted rotor sources must be recorded by the runtime campaign.

Sampling the complete OP interaction field in Tidewater would also duplicate
legacy boat wakes over Tidewater's accepted boat simulation. A possible bounded
repair is explicitly tagged rotor descriptors in a private Tidewater uniform,
with rotor-only annular slope perturbations within the same camera domain. It
must preserve current OP impulse semantics and Tidewater boat/impact behavior;
no fake ammo mass or explosion plume is justified. That repair is separate from
this liquid-population change and is not implemented or validated here.
