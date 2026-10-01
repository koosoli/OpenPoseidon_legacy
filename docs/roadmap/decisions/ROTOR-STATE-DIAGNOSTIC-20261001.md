# Read-only rotor flight and input diagnosis

Baseline `ae5051c3`. This adds one harness command in `HarnessBuiltins.cpp` and
this document. It does not alter helicopter simulation, rotor RPM, inputs,
emission, camera authority or the root-owned runtime runner.

Example serial harness request:

```json
{"cmd":"rotor_state","x":2700,"z":5150,"radius":16}
```

The command requires a single-player world and landscape, finite in-map X/Z,
optional finite radius 1..50 m (default 16 m), and at most 4096 world vehicle
slots. It selects exactly one nearest finite helicopter in horizontal range,
with RenderId tie-breaking and a candidate count. Destroyed aircraft are
reported, not silently discarded: their state may explain a failed fixture.
No match, oversized census or malformed/nonfinite state returns an explicit
error. There is no unbounded nearest-object or collision query.

The response reports actual object/class/model and simulation `timeMs`,
position, ground clearance, normalized physical RPM, engine demand boolean,
attitude up vector, velocity, fuel, total damage, destruction, locality and
airborne state. `rpm` is the real public `Helicopter::RotorSpeed`; `engineOn`
is the existing virtual `EngineIsOn`, which can differ during spin-up/coasting.
Total damage is not a claim about the inaccessible private engine hit channel.

Driver/person/brain presence, locality, life, player equality, actual pilot
unit equality, focus equality and numeric brain state accompany actual
`QIsManual`, driver `QIsManual`, `PlayerManual`, `playerSuspended` and
`cameraEffect`. The active input enum and newly resolved context/seat/transport
are both returned, allowing a stale context or wrong focused unit to be
distinguished from actual driver authority.

`moveUp`/`moveDown` use the real HeliPilot action profile with normal focus
checks. `unfocusedMoveUp` is the same read-only action query without focus
filtering; it does not bypass filtering for the pilot or feed a control.
`focusLost` and actual `qKey` accompany them. This can distinguish an accepted
SDL Q event from an unavailable/focus-filtered collective action. Local airflow
enabled/scale and camera distance are also returned. Invalid/unavailable camera
distance is explicitly tagged false with sentinel -1, not asserted in range.
Visual-effects precision and particle asset readiness are not licensed by this
query. Neither a successful response nor RPM alone accepts land emission.

The earlier recorded `original-pilot-board-20261001-015327-ad03fa` run
established a real UH60/SoldierWB pilot but used a scripted camera. Source
inspection found that `World::SetCameraEffect` sets `_playerSuspended=true`,
and `Transport::SimulateUnits` routes its manual driver to `SuspendedPilot`
instead of `KeyboardPilot`. Thus Q never reaches the keyboard collective's
engine-start path. `triGetInputContext=HeliPilot` alone cannot prove otherwise.
`triFreeFlyPose` also creates a Zeus camera effect; `triClearView` releases only
the independent render override, not a previously activated scripted/Zeus
camera. A fresh manual fixture should use normal EXTERNAL flight view and
only presentation `triSetView` for paused captures, with camera-effect zero
and actual pilot/driver/input proofs before positive time.

The revised unsuspended fixture still reported engine off and no sustained
rotor trace; its cause remains unresolved until this query is installed.
Separately, the production RPM increase is bounded by `0.05 * deltaT` in
`Helicopter::Simulate`. From zero, reaching 0.65 requires at least thirteen
simulation seconds. Lifting a stopped helicopter 18 m after a quarter-second
Q tap lets it fall before sufficient lift. Actual `isEngineOn=true` is not
proof of usable RPM. Warm up while grounded using existing engine start and
real elapsed simulation, then make an ordinary Q-controlled ascent with actual
clearance/RPM/velocity evidence. Avoid `MakeAirborne`, repeated position pins,
forced rotor speed or velocity as acceptance shortcuts.

An AI pilot sent `doStop` followed by a destination at its current X/Z may
remain in stopping/reached behavior. `UpdateStopMode` and `AIPilot` contain
actual landing/StopRotor paths. That is a source hypothesis, not the diagnosed
cause of the corrected manual run; the new numeric brain-state/authority
snapshot should decide whether the AI or keyboard path is applicable.

Author validation: `git diff --check` passes. A focused source-contract check
extracts the actual registered lambda and confirms the required diagnostic
fields, finite/radius/census gates and absence of setter, EngineOn/EngineOff,
MakeAirborne, Simulate, Advance, ObjectCollision or action-consumption calls.
This is source evidence; no translation-unit/full-game compilation, game,
GPU test, deployment or live request was performed by this agent. Root owns
compilation and the next installed diagnostic run. Existing accepted ocean
downwash is untouched.
