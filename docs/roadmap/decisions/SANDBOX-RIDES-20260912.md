# Sandbox ride iteration

Baseline installed 33aaf7c8, source 7b1b096a.

Owner requests: unreliable moving Jeep rifle fire; UH60 personal rifle (NOT FFAR),
NPC flight, more targets, enough time to ready the weapon, repeated circuits.

- Straight moving Jeep baseline 181742: 24 mouse clicks produced 24 shots at
  approximately 40-49 km/h, all sampled insertion checks clear. Does not reproduce
  the owner's intermittent issue; turning/arc-edge behaviour remains open.
- Original UH60 cargo 0 inspected in 180934: cargover1, forward-facing proxy.
- Content iteration: explicit start/stop Jeep control, 32 extra unarmed targets,
  repeated route; helicopter pilot and departure/return actions; cargo 0 rifle
  permission uses existing actions as an INITIAL FIT TEST, not accepted artwork.
- Pending installed validation: UH60 pose/collision, personal-weapon shots, flight
  and landing, Jeep circuit/start/pause and existing regression checks.
- An isolated user-mod preview did not expose the seat mapping. The package fit
  test is kept explicit rather than counting that failed load as a pose result.

Do not mark this work complete before the installed checks succeed.

## Owner stop / revised scope (2026-09-12)

Further per-seat UH60 fitting and animation experiments are PARKED at the
owner's request. No further runtime fitting or deployment is authorised by
this iteration. A generally reusable solution, not a vehicle/weapon/seat
matrix of bespoke fixes, is the requirement for resuming this work.

Current source/installed stamp: 10515826. The default package contains an
UNACCEPTED UH60 cargo 10 OPHeli pose trial; parking does not remove that trial
from the installed package. It must not be advertised as working support.
The last installed trial restored torso height and passed 24 animation checks,
but personal-rifle firing remained blocked by carrier component 55 at entry
parameter zero. Muzzle-forward visibility was clear. This is not proof that
ignoring carrier collision would be correct. Flight/landing and the new Jeep
circuit remain unverified. The straight-road Jeep result above is not a fix
for intermittent firing during turns.

Revised desired behaviour: the player may aim and fire inside the carrier,
including at its pilot, passengers and body. Carrier obstacles should receive
real projectile hits, not disable the trigger or be globally ignored. Preserve
normal ammunition, damage and authority handling. Remove artificial safety
arcs as a design requirement for this future general path; do not confuse that
with a tested implementation. Generic aiming/weapon attachment and projectile
origin handling need validation before claiming support for all vehicles or
weapons. No cheap, verified general solution has been established yet.

## Bounded general-path reconsideration (2026-09-12)

Owner authorised another short attempt, explicitly not prolonged seat fitting.
Code-path inspection, not a new runtime acceptance:

- AI/VehicleAICombat.cpp::EntityAI::FireMGun already uses WorldTransform and
  AttachedPointSpeed for attached shooters. Vehicle motion is not a missing
  general projectile facility.
- SoldierOldAI.cpp::GetAvailableCargoWeaponSeat still requires an exact cargo
  mapping. GetActiveCargoWeaponSeat additionally requires owned animations.
- SoldierOldSim.cpp::BasicSimulation uses seat angle acceptance to choose aim
  versus idle; SoldierOldAI.cpp::AimWeapon also clamps aim to those angles.
  Removing CargoWeaponLineClear alone cannot provide general free aiming.
- Soldier::FireWeapon passes the animated muzzle to FireMGun. Removing the
  insertion veto without changing origin handling can spawn beyond an intervening
  wall. Replacing it by a hit at the first insertion contact also needs a policy
  for the already observed UH60 contact at parameter zero, rather than claiming
  that a coarse carrier volume identifies an actual cabin wall.

The earlier explanation that the safety veto was the main obstacle was incomplete:
the animation/seat eligibility coupling is an independent obstacle. No engine
patch or further per-seat trial was performed in this bounded reconsideration.
General support remains PARKED, not rejected as a gameplay idea.

If reopened as an actual larger implementation: separate player weapon eligibility
from seat animation ownership, supply one generic seated aiming fallback, and
resolve the interior-to-muzzle segment in the existing projectile/hit path without
excluding carrier or other occupants. Initially personal bullet weapons and local
singleplayer only; launchers/grenades and multiplayer require separate acceptance.
Do not call this all-vehicle support without testing differently oriented cargo
proxies and enclosed cabins. The flying UH60 sandbox rifle circuit remains the
required acceptance scene, not a delivered feature.

## Reopened implementation by owner (2026-09-12)

Owner subsequently authorised implementing the shared path. Work in progress:
player-only runtime cargo policy reuses existing package transitions as a fallback
for unmapped cargo, without changing NPC permissions or enabling multiplayer.
Full horizontal aiming removes the seat arc and ordinary infantry gun-turn clamp
for this policy. Bullet origin is conservatively moved to the shooter's side of
an insertion obstruction instead of vetoing firing. Carrier geometry and other
occupants remain targets; recursive collision results for the shooter itself
are ignored. No new per-seat animations authored.

This is NOT acceptance: compilation and installed runtime tests are pending.
In particular the known UH60 start-inside collision may conservatively hit the
carrier rather than permit outward fire. Existing fallback pose height/fit in
other vehicles and extreme-angle body appearance are unaccepted. Do not describe
the fallback as finished all-vehicle support or a solution for every weapon.

### Installed shared-path results

- f359d256: 12 UH60 player shots consumed ammunition but no exterior target hit.
  Conservative convex insertion handling alone was insufficient.
- 1a2dc1f1: carrier-only two-sided visual shell ray query, with normal crew and
  scenery collision retained. Bounding-sphere rejection prevents repeated full
  shell walks once bullets leave the carrier. This is player/SP/bullet-only.
  241 assertions in 14 cargo tests passed, including two-sided finite-segment
  intersections and unchanged NPC angle policy.
- baseline-20260912-194142: first M16 shot from stationary UH60 cargo 10 hit the
  exterior target (damage 0.50623; ammunition 30 to 29). Reload animation ran and
  ammo returned to 30; initial test falsely expected OPJeepReload rather than
  OPHeliReload. Corrected test passed in baseline-20260912-195302. Six subsequent
  floor shots raised carrier damage from 0 to 0.011. Reload video is generated
  in that capture directory; this is not a performance measurement.
- 566886fb: sandbox pilot uses direct doMove route orders plus engineOn after
  land NONE. Installed FlightOnly test completed takeoff, 12 mouse-triggered M16
  shots (speed reached 121.886 km/h), return/landing and player/pilot/carrier alive
  checks. build/showcase/shared-rifle-flight2.txt and smoke/rifle-flight.png.
  A complete repeated lap was NOT observed; return was requested during leg 0.
  First-person pose/camera visibly remains draft quality.
- UAZ validation initially stopped in the harness before player takeover: it
  required active NPC FFV even though the new generic policy is player-only.
  These failures do not validate or disprove UAZ player firing. 27e6f1ea removes
  that harness assumption; NPC permissions remain unchanged.
- baseline-20260912-200356: Jeep normal mouse sweep/fire, keyboard reload,
  menu exit/reentry, Ready/Stow/Ready regression passed. Captured muzzle vectors
  change with mouse aim; this is not merely permission to fire in one direction.
- baseline-20260912-200605: UAZ generic unmapped cargo fallback passed player
  exterior hit (first shot, damage 0.50623, ammo 29), reload and save/load.
  Test aiming now uses the actual muzzle and target position, not a fixed Jeep
  direction. Earlier UAZ misses with the fixed direction were not acceptance.
  No UAZ-specific seat configuration or animation was added.

Remaining acceptance: deliberate pilot hit, proxy-heavy/different-skeleton vehicle coverage,
glass/material damage fidelity of visual shell contacts (component=-1), and
measured additional CPU cost. The shell query currently skips hidden/proxy faces;
do not assume every addon vehicle's shell is represented entirely in visual LOD0.
No claim of all weapons, multiplayer support or polished full-body aiming.

The bounded implementation is installed and functional for these measured cases,
not a completed all-vehicle acceptance matrix. Sandbox boarding still requires
the explicit Ready personal weapon action; start the pilot circuit afterwards.
The latest mission hints remove the obsolete artificial firing-arc warning.

### Passenger tour menu regression (September 12)

Owner reports tour entry vanishes after boarding. Confirmed code cause:
InGameUI::ProcessActions collects unit->GetVehicle()->GetActions, whereas both
tour commands were installed only on player. Add start/land actions to labHeli
and forward their object-local IDs through heli-command.sqs to the existing
mission handlers. No engine, pose or general action-menu behaviour changes.
Test-Showcase -FlightMenuOnly now checks actual passenger action-menu text and
executes the carrier USER actions, rather than bypassing them with exec action.sqs.
Full FlightOnly uses the same route. The earlier flight test did NOT establish
menu availability. Installation/runtime acceptance pending while owner is playing.
