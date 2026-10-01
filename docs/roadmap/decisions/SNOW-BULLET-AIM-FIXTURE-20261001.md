# Snow bullet fixture: actual direction before every shot

Status: script/source correction, helper self-test passed. No game, GPU, engine
build or deployment performed by this agent. Installed acceptance is pending
the matching read-only `player_weapon_state` command (`4991e4b6`) and root's
actual run. Script baseline is `9c2a091e`.

The installed `9527c006` campaign
`build/snow-bullet/settled-highlight-headroom-20261001-031946-3b3a37` passed its
first real dry terrain hit and its real deposited-snow cut. Its third round
produced no required covered terrain event: the recorded terminal was
`kind=expired`, at Y358.072/Z6344.500, with final velocity Y+61.565/Z373.957.
The second ground terminal had velocity Y-247.222/Z865.379. Those are actual
terminal values, not a launch-direction getter or a roof hit. The missing third
terrain event cannot prove tent-wall collision or roof exclusion. The old
fixture comment that a lower tent lift established a wall hit was unsupported
and has been removed.

Source inspection explains why its initial mouse calibration was unsafe to
reuse after photography. `InGameUI::GetCursorDirection` transforms the UI model
cursor by its current CameraFrame. `InGameUIMenuSim` sends that direction to
`AimWeaponManDir` for the manual gunner. `World::Simulate` can replace the scene
camera with `s_triViewTransform`; `triClearView` only releases its active flag.
The override is a rendering tool, but manual UI/camera adaptation can still
affect the wanted gun direction. A single initial relative mouse movement does
not prove later gun direction, especially after close/top/away/return views.
This script correction does not change those engine paths or claim to fix all
render-override/manual-input interactions.

The runner now calibrates every ordinary shot through the actual read-only
query, which returns the selected Man bullet weapon's `GetWeaponDirection`
and `WorldTransform().FastTransform(GetWeaponPoint)` muzzle. It releases the
override, switches to INTERNAL, and allows 1000 ms of real simulation before
control. The first dry shot aims in the known north heading at normalized
Y=-.274. Once that real round reaches terrain, its terminal XYZ becomes the
target point for the next shots. Each subsequent desired direction is recomputed
from the current actual muzzle to that same bare-terrain point, accommodating
changed player support after deposited snow.

The controller applies only ordinary `mouse_motion` SDL events. Yaw/pitch
errors come from the actual direction; measured finite positive response gains
adapt the next relative input. `AimDy` now seeds the initial gain (the existing
default250 is preserved); it is no longer an assumed absolute calibration.
Motion is capped at180 pixels per axis,24 iterations and25 wall seconds. Three
samples separated by actual advancing simulation must each be within .0025
radians and downward. No call sets gun/weapon state, cursor direction, bullet
velocity, physics or a snow deficit, and no extra calibration bullets fire.

After convergence the script pauses, captures the ordinary first-person UI,
reads actual gun direction again and requires the downward target match before
its original real SDL left click. The complete actual state/control history is
saved under each `*-aim` stage, including source model, muzzle, direction,
camera/manual status and simulation time. A missing or unknown query fails
honestly. The existing injected mouse-up, single Fired event, selected M16
ammunition decrement, actual downward terminal terrain trace,1.4–8 m range,
calibrated XZ patch, kernel/cell bounds and2000 ms post-release settling gates
remain intact.

The covered control still requires an actual terrain hit in the audited central
patch, `sheltered=true`, unchanged stored deficit and no patch mutation. An
object/water/expired terminal or missing terrain event is diagnostic failure,
never a successful negative. Newly recorded terminal lines remain in the result
on failure to distinguish an aim problem from a real fixture collision/cover
problem. The six-metre lifted stock roof is retained, but its projectile
clearance is pending actual verification rather than presumed from its lift.

`Test-SnowBulletRuntime.ps1 -SelfTest` passes finite/raw-XYZ vector arithmetic,
an upward gun requiring downward normal input, exact direction requiring no
motion, target-from-muzzle ordering, nonmanual/suspended/camera-effect refusal,
and existing trace/kernel/settling/exposure/restoration falsifiers. These are
controller/helper tests, not evidence of actual engine response. Root must
compile the query, run the owned locked installed fixture, inspect pre-fire UI
and actual direction history, and establish the real covered terrain/shelter
event. Hole visual quality and snowfall refill acceptance remain separate gates.
