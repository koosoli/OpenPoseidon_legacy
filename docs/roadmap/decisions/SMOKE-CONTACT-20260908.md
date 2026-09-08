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
