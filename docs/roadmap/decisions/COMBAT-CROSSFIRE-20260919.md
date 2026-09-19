# Crossfire beyond the target — 2026-09-19

Owner report: standing amid the 63MAX infantry, ballistic trails cross the player
and allies on the opposite side are still hit. The installed reference is
codex/combat-fixes 29568e0f (matching EXE/DLL from the previous acceptance).

## Cause and change

The previous pre-emission bullet guard ended at ammo.maxRange: this is an AI
engagement envelope, not a projectile lifetime or a collision. Misses continue
past the enemy and that envelope. It also tested a straight ray rather than the
falling projectile and centred GeometrySphere at VisiblePosition (the soldier's
animated aiming point), rather than the corresponding GeometryCenter.

The guard now follows a bounded forward-Euler path with the projectile's shared
config drag/gravity/lifetime, muzzle origin, inherited shooter velocity and
current optional ballistic wind. Both dispersion axes contribute to the safety
envelope. Geometry centre/radius are paired; current ally speed expands the body
for travel during flight. Each emitted burst round still rechecks fresh actors
through the actor population (excluding decals). No random draws, damage immunity, target
knowledge, aim threshold or multiplayer/manual/script firing contracts change.
The default Improved setting and the existing AI-tab Legacy switch remain.

The path ends at lifetime expiry or when its upper envelope is below terrain.
An enemy or intervening object is not assumed to stop every round. This covers
straight residual travel/penetration without treating cover as guaranteed safety;
it does not predict arbitrary ricochets or future changes of wind/ally intent.
Integration and pose padding are conservative approximations, not a mathematical
guarantee for all mod content. Initial delays and lifetimes beyond 30 seconds
withhold the shot rather than silently truncate the safety horizon.

## Verification

Build via Build.ps1, PoseidonGame and PoseidonTests. Targeted combat, ballistics,
grenade and interpolation suite: 43 cases / 488 assertions passed.
New tests cover allies at 300/600 m, curved paths, inherited velocity, dispersion,
clear off-path/behind-shooter controls, degenerate segments and unsupported TTL.
A 48-point moving/static-body matrix checks that block culling agrees with an
exhaustive segment test; clearing the retained allocation cannot retain a result.

Run-CombatAudit.ps1 -Scenario Crossfire holds the enemy at 100 m, then moves a
friend to 20 m, 150 m, 300 m and 300 m with 20 m lateral clearance. Each phase is
8 seconds of permitted autonomous fire; Fired events count actual rounds. The
fixture keeps target and friend alive so their death cannot explain cessation.

Reference run Crossfire-20260919-143446: **0 / 0 / 21 / 21** rounds.
The third phase reproduces the shipped gap; the fourth is the clear control.
Fixed run Crossfire-20260919-143642: **0 / 0 / 0 / 21** rounds.
The previously missed 300 m friend inhibits fire and clearing the line restores it.

An initial implementation queried landscape actor cells on every integration step.
It was not deployed. The dense run exposed unacceptable CPU costs; a Legacy
control also remained expensive, so the entire cost cannot be attributed to the
guard. The final implementation builds the path once, groups 16 segments into
conservative boxes, and reads each allied actor once. Capacity is retained in a
thread-local scratch buffer; no body/decision cache can permit a stale shot.
There is no spatial travel/speed cutoff omitting unusually fast or distant allies.

The passive ai_stress_state harness command only reads existing animation,
velocity, weapon direction and fire-target state. It does not evaluate GetAimed,
visibility or any skipped AI decision. -CenterPlayer positions an invulnerable
fixture player amid the original army for a bounded crossfire/performance smoke.

## Dense mission follow-up

Final batched-path run Stress-20260919-144420: 25 wall-clock seconds after moving
the player amid the army; five samples from mission time 10.319 to 19.856 show
707 living infantry throughout. Moving infantry (speed >0.2 m/s) range from 641
to 262; player fire-target holders grow from 0 to 231. No sampled weapon has
barrel Y >0.5. This does not establish that an intermittent sky-shot never occurs.

Legacy run Stress-20260919-144040, same centre fixture: mission time 10.812 to
17.170, living infantry 707 -> 687; 642 -> 337 moving; 0 -> 219 player-target
holders. The sole WEST player is invulnerable and does not fire. The reduced
casualty count supports the crossfire fix, but no per-hit attribution was added.
Different simulation progression and surviving populations prevent a clean FPS
comparison; there is no new vanilla-performance parity claim. The earlier
cell-per-step experiment Stress-20260919-143809 was much slower (mission time
1.904 -> 5.440) and was discarded. All three harnesses exit cleanly.

Stationary/turning infantry and incomplete simultaneous player targeting occur
in both modes. This bounded observation does not diagnose every reported
locomotion/aim issue or the owner's far-versus-near framerate difference.

## Installed acceptance

Production commit **7c3d528a**, built from the committed tree, pushed on
codex/combat-fixes and parity/foliage-lighting-001, deployed under the game lock
via Deploy.ps1 at 14:48. Both installed payload hashes match staged files:

- EXE: `624869AB2498AFCEA8528EC65497C7303B986D9F72F845CF4A36361437ADFBD8`
- DLL: `FCE987B1DC02F17CD9AB1949623D9AF8EAB46ABB8329806BF69026F27683434A`

Crossfire-20260919-144906 uses an isolated copy of that final installed pair:
**0 / 0 / 0 / 21** rounds. WGPU initializes as build 7c3d528a910d-release;
clean harness/process exit, no script errors, renderer validation errors or panic.
No test process remains. The parity worktree's existing Cargo.lock modification
retains SHA256 9A4E74D791B26A4303E064867DA67B862C1439D0635DF3E19ED5A97AADF31A2E.
The following documentation-only commit records acceptance without changing the
installed production source.
