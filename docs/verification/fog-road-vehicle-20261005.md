# Fog roads and bounded mud vehicle follow-up

Code: `7a79e655`, `c16bc5ac`, `c179034b` on
`codex/kolgujev-fog-corpse-fixes-20261004`. Owner fog report takes priority;
this follow-up ends at installation and bounded verification.

## Fog

Both layers are enabled by default again (second extinction 0.003).
Native terrain determines the outdoor fog domain. An unavailable or out-of-map
camera-centred rain/shelter source now means unknown outdoor air, rather than
an abrupt zero-density rectangle. Known roof depth still excludes the medium;
native terrain, camera validity and underground gates remain in force.

Projected black shadows now transport their colour through fog before alpha
compositing. That correction alone did not remove the reported road rectangle.
An additional shadow-map ablation preserved it, ruling out the original shadow
hypothesis for that rectangle.

At original Everon `[9182.32,2053.96,77.15]`, az316.6/el-11.9, the road's
interpolated vertex chord lay below the fine terrain. Its fragment depth was
terrain-conformed, but its lighting position was classified as underground:
sunlight disappeared and the receiver skipped fog. Proven ground receivers now
classify cave exposure at their seated height. Actual cave meshes and bridges
retain their original underground/aboveground handling. Before/after captures
at the same camera show that the near-black road rectangle disappears.

- Actual GPU layered-fog tests: 14 passed, including moving/unavailable shelter
  domains, roof exclusion, solar transport and fogged shadow compositing.
- Complete terrain, grass, direct and retained shaders compose and validate.
- Original Everon road: 13 actual camera poses, fog0.78, temporal rendering,
  normal exit and no GPU/runtime failures.
- Original Kolgujev compact band: 26 camera poses over coast/ocean, normal exit
  and no GPU/runtime failures. Reviewed endpoints have no reproduced rectangular
  rain-map boundary. This is not proof of universal flicker-free rendering or
  a matched performance improvement. The compact test intentionally disables
  layer1; the Everon default test retains both layers.

Local captures: `build/fog-flight/everon-road-before-20261005-133140`,
`everon-road-after-20261005-135138`, `everon-road-csm-20261005-135432`,
`everon-road-seated-20261005-141040`, and
`kolgujev-compact-native-domain-20261005-135841`.

## Vehicle prototype

Stock Jeep wheel deformation is already default. No flag is needed. Two new
single-player experiments are opt-in:

- `POSEIDON_MUD_TRUCK_PROTOTYPE=1`: exact stock `Truck5t`, six mapped wheels,
  wet-layer deformation capped at16cm.
- `POSEIDON_MUD_TRACTION_PROTOTYPE=1`: bounded longitudinal rolling resistance
  for admitted wet-soil wheels, including the stock Jeep. This is not lateral
  tyre slip, a new engine/torque model or a full tyre solver.

Installed real-rain/AI-driving tests passed:

- Truck+resistance moved8.79m, approximately14cm negative rut height,
  actual6/6 admitted-wheel resistance traces around0.94m/s².
- Default Jeep moved8.66m, approximately10cm negative height, no resistance
  trace; dry contacts produced no chunks.
- Jeep+resistance separately passed. First attempt reached wetness0.824 but
  exceeded the60-second real-rain deadline. A90-second budget passed the same
  wetness0.85 admission requirement; no forced-wetness stamp was substituted.
- Wheel schedule/admission and vehicle policy unit assertions passed, including
  stationary, airborne, teleport, dry/refused contacts, SP flag isolation,
  signed reverse driving and bounded dissipative force.

The initial two integration runs have an explicit `+dirty` deployment tag:
the only tracked dirty file was the runtime PowerShell script, not engine or
renderer source. The final install is rebuilt/restamped after committing scripts
and this evidence. Exact per-run stamps are preserved in the receipt.

`scripts/Show-MudVehiclePrototype.ps1` launches an isolated, drivable original
Nogova field after natural rain wetting. It holds the game lock until exit.
For the truck experiment, run from this worktree in PowerShell:

```powershell
& 'C:/Program Files/Git/bin/bash.exe' -c 'LOCK_OWNER="Truck mud demo" scripts/with-game-lock.sh pwsh -NoProfile -File scripts/Show-MudVehiclePrototype.ps1 -VehicleClass Truck5t -Resistance'
```

For default Jeep ruts, use `-VehicleClass Jeep` without `-Resistance`.
Numpad Enter switches third person; drive/reverse over the same tracks, then
get out to inspect them. Startup flags affect only that process. The launcher
uses the existing test mission and spawns one vehicle, avoiding the earlier
local demo's duplicate Jeep. Wider handling, coasting A/B, refusal surfaces and
multiplayer rollout are not established by these bounded tests.

## Boot/tyre detail boundary

No sole or tyre parallax assets were added. Existing mud deformation already
feeds both geometric height and normals. Its12.5cm field cannot resolve fine
boot tread, and the legacy footprint decal can coexist with deformation.
The proposed next visual experiment is a sole-shaped depth map with matching
normals and ground-colour preservation; normal mapping alone changes lighting,
not depth. Directional tread storage and parallax integration are a separate
task, not silently included in this prototype.
