# Stock Jeep mud-wheel prototype

Scope: stock class `Jeep`, local physics only. No generic vehicle rollout,
traction tuning, imported assets, soft-body solver or multiplayer replication.

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
- `scripts/Test-MudField.ps1`: existing mud geometry/admission-policy assertions
  passed after extracting shared admission.
- Actual `Car.cpp` and `MudContact.cpp` CMake object targets compiled with the
  Windows clang rwdi configuration.

Runtime remains a required integration check. Suggested test: standard Jeep on
open soft ground in Everon after sustained heavy rain; drive one line, inspect
the four-wheel ruts, reverse along it and check suspension/steering stability.
Repeat on roads, under roofs, over snow and in dry weather to verify refusal.
`POSEIDON_MUD_TRACE=1` logs `MUD_WHEEL` for actual field mutations.

## Contact integration limit

The near-car `GroundCollision` path uses `SurfaceY`/`BumpySurfaceY`, whose terrain
height and derivatives already include the mud/sand field. The far-car
`GroundCollisionPlane` path builds one support plane from `RoadSurfaceY` near
the centre of mass; it does not resolve individual narrow ruts. This change
does not rewrite that existing precision choice or claim universally exact
wheel/collision coupling. Runtime testing must establish the driven Jeep's
behavior before any wider vehicle rollout.

Existing terrain rendering receives the mud field. No new tire-profile texture
or parallax shader was added. Visible rut shape and vehicle response must be
judged in the installed merged build; the subagent did not deploy or run a game.

## Installed integration check

Root installed21d85d8c and ran scripts/Test-JeepMudRuntime.ps1 on original Nogova cultivated soil with real rain and AI driving. The stock Jeep moved8.49m, produced actual MUD_WHEEL mutations and roughly10cm negative height offsets. Dry contacts produced no chunks. Close capture checked in root review; tyre-profile polish, re-driving/steering and all refusal surfaces remain outside this bounded runtime smoke. Final488a93e7 changes only fog sampling and test scripts after this Jeep check. See fog-grass-jeep-20261005-runtime.json for source/provenance.
