# Stock Jeep mud-wheel prototype

Scope: stock class `Jeep`, local physics only. Jeep deformation stays default.
No generic vehicle rollout, imported assets, soft-body solver or multiplayer
replication. Two additional experiments are opt-in and single-player only:

- `POSEIDON_MUD_TRUCK_PROTOTYPE=1`: exact stock `Truck5t`, existing six-wheel
  selection mapping, capacity 65% of the wet layer capped at 0.16 m. This does
  not activate other truck variants or inherited/mod vehicle classes.
  Selection evidence: `CarType::InitShape`/`ScanWheels` already map the front,
  middle and rear left/right Czech wheel selections in the actual contact LOD;
  only mapped grounded points participate, rather than fabricating six contacts.
  The stock `Truck5t` is also used by `dev-missions/freefly-lights.eden`.
- `POSEIDON_MUD_TRACTION_PROTOTYPE=1`: modest longitudinal rolling resistance
  on admitted wet-soft-soil wheels. Contribution scales with supported wheels,
  layer softness/depth, caps at 1.2 m/s² and cannot reverse velocity by itself.
  No lateral-slip/torque/engine tuning or full tyre solver is claimed.

Flags are read at process start. No flags means the already verified baseline.
Multiplayer ignores both new experiments even if the environment flags are set.

The car collects solid-ground collision points for identified wheels. A car-wide
land-contact flag cannot stamp unsupported wheels. Roadway-object collision
points are excluded, and any water contact clears the wheel trails. Stamps run
after collision evaluation, so every wheel force in a simulation step sees the
same pre-stamp field.

Wheel anchors occur every 0.25 m along the travelled path, with a maximum joined
segment of 4 m. Standing does not repeatedly stamp; airborne/refused intervals
and teleport-sized jumps break the trail. The existing infantry admission is
shared: original OFP soft soil/cultivated masks, valid bounds, sufficiently wet,
no road support, physical roof shelter, shore, excessive slope or snow cover.
Each intermediate anchor is admitted independently. Wheel capacity is half the
current rain-softened layer depth, capped at 0.12 m, with bounded compression on
each pass. This is a conservative prototype, not a decimetres-deep truck model.

## Verification

- `scripts/Test-MudWheelTrail.ps1`: passed dense/sparse simulation schedule
  parity, stationary, airborne, teleport, invalid input, refused intermediate
  anchor and repeated bounded compression assertions.
- Vehicle policy assertions cover exact-class/opt-in/single-player isolation,
  dry/no-support/rest refusal, partial support, reverse driving, acceleration
  cap and dissipative/no-reversal behavior over varied speeds/time steps.
- `scripts/Test-MudField.ps1`: existing mud geometry/admission-policy assertions
  passed after extracting shared admission.
- Actual `Car.cpp` and `MudContact.cpp` CMake object targets compiled with the
  Windows clang rwdi configuration.

Runtime remains a required integration check. Suggested test: standard Jeep on
open soft ground in Everon after sustained heavy rain; drive one line, inspect
the four-wheel ruts, reverse along it and check suspension/steering stability.
Repeat on roads, under roofs, over snow and in dry weather to verify refusal.
`POSEIDON_MUD_TRACE=1` logs `MUD_WHEEL` for actual field mutations.
Repeat with the stock 5t truck and truck flag for wheel selection/steering
validation. For rolling resistance, compare the same wet Jeep driving/coasting
path with the traction flag absent/present; road/dry behavior should match.
`POSEIDON_MUD_TRACE=1` additionally records once-per-second `MUD_RESISTANCE`
with admitted wheel count and actual applied longitudinal deceleration.

## Contact integration limit

The near-car `GroundCollision` path uses `SurfaceY`/`BumpySurfaceY`, whose terrain
height and derivatives already include the mud/sand field. The far-car
`GroundCollisionPlane` path builds one support plane from `RoadSurfaceY` near
the centre of mass; it does not resolve individual narrow ruts. This change
does not rewrite that existing precision choice or claim universally exact
wheel/collision coupling. Runtime testing must establish the driven Jeep's
behavior before any wider vehicle rollout.

Existing terrain rendering receives the mud field. No new tire-profile texture
or parallax shader was added: directional tyre-profile storage and shared shader
integration would exceed this bounded follow-up and conflict with fog work.
Visible rut shape and vehicle response must be
judged in the installed merged build; the subagent did not deploy or run a game.

## Installed integration check

Root installed21d85d8c and ran scripts/Test-JeepMudRuntime.ps1 on original Nogova cultivated soil with real rain and AI driving. The stock Jeep moved8.49m, produced actual MUD_WHEEL mutations and roughly10cm negative height offsets. Dry contacts produced no chunks. Close capture checked in root review; tyre-profile polish, re-driving/steering and all refusal surfaces remain outside this bounded runtime smoke. Final488a93e7 changes only fog sampling and test scripts after this Jeep check. See fog-grass-jeep-20261005-runtime.json for source/provenance.
