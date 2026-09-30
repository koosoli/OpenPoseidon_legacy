# Infantry muzzle flash illuminates actual nearby geometry

The original Soldier::FireWeaponEffects bullet path already owns one
WeaponLightSource, but calls Start(.05,.004,true). LightPoint::SetBrightness is
a radius setter (50 times its argument), so this produces only about 0.2m core
and 2m reach. It first registers during later Man simulation, and lacks the
daylight-visible flag, so daytime dark buildings can have no muzzle illumination.
The source also runs at infantry simulation precision as low as 5Hz, making a
short flash's visible lifetime dependent on a later AI update.

The correction reuses that source rather than adding a second light. The
successful AmmoShotBullet effects event resolves GetWeaponPoint(weapon), which
uses the active weapon muzzle memory point and animated weapon proxy transform,
then publishes one actual point light immediately at its world position.
Attached owners use the same WorldTransform convention as other muzzle queries.
The pulse belongs to the fired pose/weapon rather than a weapon selected after
the shot. Bullet geometry, recoil, damage, ammunition, muzzle sprite phase,
smoke and sounds are unchanged. Other vehicle/explosion light sources retain
their existing path.

The point light has warm bounded diffuse/ambient colour, 0.7m core, 8.4m finite
reach, a 20ms peak and quadratic falloff to zero at 110ms. Integer simulation
milliseconds avoid frame partition and long-session floating clock drift.
Burst events retrigger the same light, without accumulating energy or adding
scene entries. Default light IsOn behavior is unchanged; the muzzle subclass
checks the pulse clock in base dispatch, descriptions and CPU shading. Expired
pulses therefore leave snapshot candidate selection even between AI ticks.
The existing renderer snapshots and point-light shading already illuminate
the shooter, weapon, terrain and surrounding object surfaces; no new shader,
public renderer ABI or generic light-selection policy is introduced.

Legacy 150ms source housekeeping and its existing cosmetic random-call schedule
are retained separately from visible 110ms energy. This avoids shifting the
shared gameplay random stream. The source frees its reference on the existing
cleanup tick; destruction releases it and the Scene's weak Link slot becomes
reusable. Save/load retains the same three archive fields, storing an active
cosmetic muzzle pulse as inactive so it cannot reload as a legacy 50m light.
No pulse save/network identity or persistent gameplay field is added. Real
network-delivered firing effects retain the existing event route.

POSEIDON_MUZZLE_LIGHT=0 restores the exact old source behavior for matched
comparison. POSEIDON_MUZZLE_LIGHT_TRACE=1 logs the actual bullet event, simulation
milliseconds, actual world muzzle, immediate publication, lifetime/core/reach.
The engine's existing LIGHT_EXPLO switch still controls this light. Ordinary
local-light budgets/selection and geometry shadow limits still apply; this
does not claim a new all-world light or shadow system.

CPU standalone execution of the actual pulse header passes 410 sampled ages,
including bounds, expiry, clock reversal, nonfinite/negative strength, burst
retrigger and finite reach. New [muzzle-light] Catch2 cases exercise the actual
MuzzleFlashLight through Light base dispatch, actual emitted descriptions,
warm colour, exact muzzle coordinates, expiry without an AI tick, pause reads,
retrigger/no accumulation, switch-off and unchanged ordinary light defaults.
Those linked production-class tests and the game build require root execution;
no game/GPU/build or installed appearance acceptance is claimed by the agent.

Installed acceptance must use real standard-US player and AI successful shots
at night and inside a dark daytime stock shelter, with renderer/local-light
settings held fixed. Retain actual bullet/muzzle trace, weapon ammunition
decrement and source model/camera, plus before/peak/expired images of the
shooter body, weapon, nearby ground and an actual nearby wall. A white muzzle
sprite alone does not prove surface illumination. Compare the same event/pose
and simulation age with the legacy ablation, and test burst retrigger, low AI
update cadence, paused pose, stance/prone positions, expiry, actor deletion and
mission unload. Existing light/flash/particle source tests must remain passing.
Actual player visibility and stock wet-cloth/mud fixes are separate pending
dependencies for judging that player's body response.
