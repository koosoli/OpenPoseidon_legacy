# Smoke contact follow-up (2026-09-08)

Base f007ea89; bounds implementation eb0981a9; asset-obs worktree only.
Foreign Cargo.lock is excluded and unchanged.

The previously measured 27% simulation improvement is still present. It measured
the conservative broad phase with 8x outdoor smoke and wind 18 m/s, not every
indoor or helicopter-downwash situation. Do not generalize that number.

## Fixed data paths

SmokeVolume's sprite draw already limited puff size by room clearance and contact
planes. AppendShadowBlobs, reused for volumetric injection, ignored both and sent
the entire growing radius. All three consumers now use SmokeParticleRenderRadius.
Known contact surfaces no longer deliberately allow 10-15 cm of visual bleed.
Puffs clipped below the injector's 5 cm minimum are omitted rather than inflated
back through geometry. Outdoor unconstrained radii remain unchanged.

The cached collision offset includes half the radius at the time of impact.
That padding is now recorded with the plane: substituting the larger CURRENT
radius while rendering previously expanded the apparent clearance as smoke aged.

The room-radius result is shared across the three consumers, invalidated when
the particle is integrated or live parameters change. This avoids multiplying
room queries by the number of render consumers. No extra collision ray, particle,
FFT, volume grid or ray-march step is added. WindModel and AirflowField consumption,
shelter and the simulation's collision schedule are unchanged.

## Checks and honest limits

- Bounds tests cover open air, room clearance, zero clearance, roof and wall
  planes, ageing after contact, disabled containment and zero new sphere sweeps.
- Targeted bounds/density/Airflow/Wind: 28197 assertions / 23 cases passed.
  Includes downward rotor wake, ground outflow, stopped rotors and wind response.
- PoseidonGame and PoseidonTests build. Matched eb0981a9 EXE/DLL installed using
  Deploy.ps1 and the game lock at 20:48, then successful installed-game captures.
- Final shared-cache build 700246eb installed at 20:54 (EXE 20:53:45, DLL
  20:53:30). smoke-wall-final completed normally at 30 seconds with the same
  settings. No engine panic/exception was found. Late instantaneous simulation
  samples range from 12 to 44 ms; sampling is too sparse for a p95 claim.
- Nogova hasic_zbroj at 7731.6/25.3/4421.4: 8x smoke, wind 18, volumetric mode 1,
  30-second before/after captures from 7764.82/4408.71/30, heading 290, pitch -5.
  Private smoke-wall-before/after files in .tmp-nogova. The building is visible
  and opaque, but exterior smoke remains: this is NOT proof of zero wall leakage.
- Indoor simulation samples remain very spiky (several samples above 50 ms at
  about 3900 particles). The short tests do not establish an indoor performance
  win; the shared-radius cache avoids the additional repeated work introduced
  by feeding bounds to every pass, not the pre-existing simulation spikes.
- .tmp-rotor-smoke/smoke-downwash: smoke at 7750/24/4460, 4x, wind 18, running
  and stopped helicopters. Logs show running RPM 1 and stopped RPM 0, drifting
  plume centres and continuing simulation. This is a coexistence check, not a
  controlled downwash-only visual A/B or proof of the remaining rotor visibility.

Teardown is a visual direction requested by the owner, not an implemented new
fluid solver. No wholesale replacement or higher-cost volumetric default was
enabled. Remaining work: exact sphere/solid collision rather than the seven-ray
approximation, voxel-field bleed at thin walls, identifying the indoor simulation
spikes, and assessing richer rolling smoke without increasing the default budget.

## Source-room follow-up

IndoorContainStep now looks up the building at the source, not the destination:
an endpoint beyond a wall is not evidence that the particle was outdoors. The
caller passes the actual integrated position instead of a two-tick lookahead.
The old lookahead could assign an outdoor/different room to a particle that had
not crossed a portal yet, dropping its current room's render-radius bound early.
No additional sweep, wind/downwash change or larger particle budget is involved.

PoseidonGame/PoseidonTests build passed. BuildingInterior, render bounds, density
and local-light tests: 758 assertions / 57 cases passed. New source-room model
coverage pins an indoor step and a solid-roof crossing whose destination is
outside. It does not exercise live building-instance lookup or prove every door.

Installed-game verification is pending: the game lock refused deployment at
22:18 because the owner has a running instance. That instance is untouched.
The existing 12-metre building cache still lacks explicit per-particle building
identity; multi-building ambiguity and indoor performance remain open.

## Sort-depth caching

BuildSmokeDrawOrder computes the camera-space depth once per particle, then sorts
indices using the cached floats rather than transforming both particles on every
comparison. Scratch storage is retained between draws (one extra float per
particle); it is rebuilt for every camera, not reused across different views.
No GPU resource, density, particle count or motion change is involved.

The production helper matches the old comparator's exact index order for 4096
particles, including equal depths, at three camera headings; empty input is also
covered. Combined targeted tests: 766 assertions / 58 cases. PoseidonGame and
PoseidonTests build passed. Installed timing/visual verification still waits for
the free game lock; no measured total-frame speedup is claimed.

## Installed audit, 2026-09-09

The previous pending-deployment notes are historical: both source-room and sort
changes are included in the installed audit builds. Additional fix `8f2e8a65`
requires the nearby cached building's actual room grid to contain the queried
point. Proximity within 12 m alone could return a different building or an
outdoor point as the cached interior. A synthetic two-building/outdoor/null test
adds eight assertions. The validation is a room-grid lookup, not an extra sweep.
Fallback lookup can run more frequently when the old cached answer was wrong;
do not call that a guaranteed indoor performance win.

The first post-deployment Nogova run crashed BEFORE test-smoke creation in
EngineWgpu::DrainInstanceOps. An unchanged retry passed. Inspection found producer
vector growth racing consumer slot accesses. `a4d09dbe` separates producer handle
allocation from consumer-owned translation storage, and rejects invalid mappings.
This is an independently exposed renderer defect, not evidence that the smoke
cache fix caused the startup crash. The ownership contract test is source-level,
not a thread sanitizer. Game/tests build passed; combined targeted suite passed
98 cases / 29,353 assertions, including the correctly named `[body-water]` tag.

Installed `a4d09dbe` completed four runs with orderly shutdown and no logged
unhandled exception/panic: Nogova 30 s sprites, Nogova 30 s volumetric, two native
Reforger Village 90 s captures (each 22,360 registered instances, complete terrain
and settled preparation). This is bounded regression evidence, not a proof
against all concurrency schedules.

Nogova uses source 7731.6/25.3/4421.4, thickness 8, wind override 18/250/0.2,
camera 7764.82/4408.71/30, heading 290, pitch -5. Correct mode switch is
`POSEIDON_SMOKE_SYSTEM=0` or `1`. An earlier audit control mistakenly set unused
`POSEIDON_SMOKE_RENDER_MODE=1`; that control was DEFAULT SPRITES, not volumetric.
Do not use it to claim a volumetric before/after. Actual final volume image
`audit-instance-volume.png` still shows exterior smoke around the building.
Late individual sim samples ~9.8-13.9 ms / ~3900 particles are not a p95 or a
controlled performance comparison. Thin-wall leakage and indoor spikes stay open.

Raw local logs/images: `.tmp-nogova/audit-*`, `.tmp-bulb/audit-final-tests.log`,
`.tmp-bulb/audit-instance-build.log`,
`build/native-reforger-perf/Village-audit-instance/`.
