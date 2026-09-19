# Combat, distance and performance audit — 2026-09-19

This document records the original investigation. The owner subsequently
authorized implementation; see [the follow-up](COMBAT-FIXES-20260919.md) for
changes, corrected grenade test range and current validation results.

Status: investigation, diagnostic/test changes only. No gameplay fix or deployment authorized by the attached audit request. Renderer investigation is on independent branch render/shot-effects-shadow-audit-001.

## Identity and scope

Requested parity/foliage-lighting-001 resolved to **29f3b479c6256b742480d84b1cae43c695ffc929**, not the attachment's 4a2a30b4. The intervening commits are 4ac8199c (helicopter full-rate path, target membership optimization, addon warnings and mission), f380053b (handover), c5d5781a (upstream inventory), 29f3b479 (PowerShell launcher repair). All source references below were re-read at this baseline; the GetAimed wrapper adds 12 lines in this audit branch.

AI branch/worktree: ai/combat-audit-001, .claude/worktrees/combat-audit. Renderer/parity worktree left untouched. Read CLAUDE.md and the root .agents instructions; older deployment/main guidance is superseded by the explicit audit baseline and prohibition on changing the owner's installation.

Built PoseidonGame using scripts/Build.ps1, win-x64-clang-rwdi, separate build directory. Runtime launches hold with-game-lock.sh, use an isolated executable directory, retail data via -C, separate POSEIDON_USER_DIR, WGPU, 1280×720, preset 3, MSAA 4, DLSS off, vsync off, cap 60, fixed exposure .15, legacy smoke mode 0. Each run records args, binary hashes, install stamp and environment in build/audit-runs/*/provenance.json. The reference is a read-only copy of the owner's already installed EXE/DLL, stamped f380053b; HEAD's later commits only changed scripts/docs. Do not confuse the rebuilt diagnostic EXE with this unchanged binary reference.

Runtime matrix and performance numbers are recorded below as runs finish. No comparison to a freshly run vanilla binary is implied.

## Active pipeline — source-confirmed

| Stage | Producer / input / result |
|---|---|
| Perception scheduling | Target.cpp:490 AddNewTargets; :710 TrackTargets; :1340 tracking scheduler. New-target period normally 5s without long-range IR (otherwise 1s); track period 1s; both ×5 outside tactical/IR enemy range. Near-target scan has its own 100m path (:1330). Group target lists share reports; known-to-this-unit and uncertainty still matter. |
| Knowledge position | Target.cpp:153 ExactAimingPosition uses live animated object point while lastSeen is less than 1s old, otherwise position + speed × elapsed clamped to 10s. AimingPosition (:254) adds posError. LandAimingPosition (:236) uses live point in the fresh case, otherwise adjusts the extrapolated/error point to preserve height over terrain. “Exact” does not always mean exact live geometry. TrackTargets stores AimingPosition and can set lastSeen to now + tracking period (:1237), so negative age can be intentional. |
| Fire visibility | Target.cpp:1390 VisibilityTracker::Value. Cache expiry controls query; sensor camera for weapon <0, otherwise current world muzzle, attached transform where applicable. Requires sensor visibility timestamp within 30s and range eligibility, then Landscape::Visible(..., ObjIntersectFire). KnownValue is not equivalent to a fresh ray. |
| Weapon / target choice | TargetFire.cpp:1668 SelectFireWeapon respects user stop, forced-fire state, target completion/knowledge and acquisition/switch schedules. BestFireResult (:999) evaluates modes; WhatShootResult Target.cpp:1562 checks ability, target state, magazine, range/lock, benefit/cost. Visibility below .63 rejects; otherwise its square scales a configured probability term. Range and lead penalties also apply. These are not exposed-body percentages. |
| Aim direction | SoldierOldAI.cpp:546 CalculateAimWeapon uses Target::AimingPosition, skill-dependent range error beyond 40m, world muzzle, target speed bounded by maxLeadSpeed × ability. Ordinary bullets use Ballistics::SolveLevelShot when enabled; time affects lead, drop affects elevation. Walking shooter prediction differs from attached carrier point velocity. AimWeaponAI / AimWeapon apply gun/body limits, dexterity, smoothing and existing RNG-driven inaccuracy. |
| Trigger evaluation | SoldierOldAI.cpp:1525 wrapper → overload. Invalid weapon/target, disabled weapon, visibility <.3, lastSeen older than 5s, posError > twice indirectHitRange reject. Visibility uses _currentWeapon, whereas subsequent mode, muzzle and direction use requested weapon. AIFire selects the mode immediately before aiming, so the normal path attempts to align these; universal equality across all callers is not established. |
| Miss tolerance | GetAimed predicts impact, compares miss with VisibleSize × ability tolerance plus range-scaled dispersion. Error >=2× tolerance rejects; <= tolerance can receive the visibility score; between is graded. Ordinary bullets scale error by .75. This explicitly disproves “mathematically exact alignment required.” Angular/body-turn constraints are separate. |
| Permission / firing | SoldierOldAI.cpp:1787 Soldier::AIFire → SelectFireWeapon → AimWeaponAI → prepare-only gate → loaded && ready && aimed >=.5 && !WeaponsDisabled → GetAIFireEnabled → FireWeapon. AIUnitImpl.cpp:2232 IsFireEnabled respects hold-fire or the specifically enabled target. |
| Emission | Man::FireWeapon in SoldierOldAI.cpp (ordinary bullet branch around :1230) → CargoBulletOrigin / weapon direction → EntityAI::FireMGun VehicleAICombat.cpp:2016 → NewShot, dispersion, transform/velocity, world/network registration. Base FireWeapon :2447 reports effects and updates ammo/burst state. |
| Authority | Soldier::Simulate chooses manual controls, remote FakePilot, disabled-animation pilot, or local AIPilot. FireMGun refuses projectile creation when effective gunner person is remote. Burst continuation is IsLocal guarded. This is local-authority execution with network effects, not universal server AI or deterministic lockstep. Multiplayer not runtime tested. |

Scripted fire / _forceFireWeapon has distinct semantics and can bypass normal selection; player manual fire uses the same lower emission facilities. A future autonomous safety patch must identify autonomous versus intentional script/manual context without silently redefining fire/doFire or player behavior. Existing DAAnim and target/movement controls, hold fire, weapon readiness and action constraints must remain authoritative.

## Cover and geometry — source-confirmed limits

Landscape::Visible's object overload is in **World/Simulation/Collisions.cpp:2077**, not solely Terrain/Visibility.cpp. It uses live object AimingPosition and max(.01, VisibleSizeRequired × reserve × .8). The underlying :1846 path estimates terrain coverage over grid cells (CheckUnderLand); the remaining visibility is multiplied by object visibility.

CheckVisibility (:1538) checks collision candidates, OcclusionFire/View, relevant LOD intersections, excludes shooter/target including self proxies, and accumulates obstruction. It is **not** multiple independently selected body-part aimpoints, but also **not** just one binary ray for the whole visibility result. Terrain coverage and a wall intersecting the preferred point can therefore disagree legitimately.

Object.cpp:2089 maps model AimingCenter to world, falling back to position for no shape. ShapeLOD.cpp:1565 initializes aiming center from geometry bounds, otherwise visual bounds, then overrides with memory point zamerny. ManType loads zamerny / osa mireni (SoldierOldSimProxy.cpp:1493), and Man returns its cached animated aiming position (SoldierOldActions.cpp:2391), updated by SoldierOldSim.cpp:265/:287/:1283. Missing/incorrect memory points or imported collision/view/fire LODs remain plausible content defects. A visual capture alone cannot validate collision geometry. No content defect is established here.

A hidden remembered target retains extrapolation/uncertainty. Fire behind cover is not automatically wrong merely because the target was previously observed. “Never known” and “known then hidden” must remain separate controls.

## Ballistic consistency — source-confirmed mismatch, gameplay impact needs controls

CalculateAimWeapon's enabled bullet path calls SolveLevelShot; Man::GetAimed still uses distance / muzzle speed and a vacuum-style gravity drop. Generic EntityAI::GetAimed already has a corrected path; infantry overrides it. The mismatch exists in the active branch. It is not yet proof that any given missed shot or partial-cover rejection was caused by it.

Both include estimated range and lead limits, but evaluator and predictor differ in flight time/drop and shooter motion. Actual shots use Ballistics::ShellParamsFor via ShotShell and integrate drag; optional wind, integrator cadence and transforms also matter. Compare actual mode/ammo, aim direction, miss score and shot trajectory at the same range before changing the evaluator. Do not lower .63/.3/.5 thresholds or globally disable drag as a remedy.

Ability comes from AIUnit.cpp:1375 (player/UltraAI forces 1; otherwise configured ability). SoldierOldAI.cpp:317 uses ability and weapon dexterity for tracking, invAbility for inaccuracy, range error and tolerance; FireMGun adds mode dispersion × AI dispersion coefficients. This is engine source, not Arma 3 skill documentation. Not all difficulty-to-ability configuration loading is runtime verified.

## Mandatory direct-fire safety verdict

**Source-confirmed incomplete protection.**

CheckFriendlyFire (VehicleAICombat.cpp:1555) is called from infantry GetAimed's unguided and guided branches. Its actual implemented geometry applies only to AmmoShotBullet; other ammunition returns true. CheckNearFriendlies (:1482) uses the shooter's center IsFriendly(target side), so it is not restricted to the same squad: player, allied groups and occupied friendly vehicles can be candidates under existing side rules. Civilians/neutrals are not automatically reclassified as allied.

Ordinary on-foot origin is **Position()**, not the muzzle. Direction is weapon direction. It considers entity aimpoints, not body extent. Maximum distance is 50m, capped to target distance. Radius is 2m, drops to 1m for targets inside 50m, .5m inside 25m. Distance prefilter is from entity position. No dispersion/aim-uncertainty envelope or penetrating continuation beyond the enemy is tested. It can miss a body even when the actual muzzle path intersects it; near-target geometry can be especially inconsistent because the line originates at the feet.

**Freshness bypass:** VehicleAICombat.cpp:1154 burst simulation calls FireWeapon directly while _burstLeft >0. It bypasses AIFire/GetAimed and hence the only normal infantry friendly-fire check. Man::FireWeapon/FireMGun do not independently revalidate it. A cleared first round does not guarantee safety for an ally entering during the burst. This is source-proven control-flow behavior; a synchronized crossing-in-burst runtime reproduction is still required before claiming its observed incidence.

Actual shot penetration is modeled in Shots.cpp; the narrow safety check neither follows its residual path nor predicts ricochets. A future bounded envelope must account for body extent, real current muzzle and relevant surviving projectile travel, not introduce an arbitrary cone or per-bullet unbounded world scan.

## Mandatory grenade verdict

**Source-confirmed: collateral is a tradeable cost, not a safety veto. There is no release-time allied-area check.**

WhatShootResult Target.cpp:1778 considers collateral only when result gain >0 and secRange = indirectHitRange × indirectHit /20 >=4. It searches the group's target list, skips vanished/destroyed/unknown-to-unit entries, and uses distance around the enemy's AimingPosition. Friendly/civilian damage adds cost with factor 6; enemy damage adds gain. The returned boolean is gain >0. Selection can trade these costs against benefits. This is not a guaranteed veto, does not explicitly cover the thrower, and does not construct a conservative landing/fuse/movement area.

Man::FireWeapon starts MFThrowGrenade for shell/smoke/illuminating ammo with magazine initial speed <30. ActionContext stores the weapon index. SoldierOldMove.cpp:92 dispatches to Man::ThrowGrenadeAction (SoldierOld.cpp:1395), which checks controls-paused/mode/ammo then FireShell with the current weapon point/direction and null target. There is no new allied-area/hold-fire validation there. Earlier selection is separated from actual release by animation; any later abort must act **before** FireShell, never claim to retract an emitted grenade.

Damage source: Collisions.cpp:1210 scans out to 4×indirectHitRange plus candidate object radius, computes occlusion using ObjIntersectIFire, then Object::IndirectDammage → LocalDammage (Dammage.cpp:119). indirectHitRange is not a hard safe boundary; falloff, hit value, armor/body extents and damage cutoff matter. Walls have modeled attenuation, not guaranteed blast immunity. No individual fragment-projectile spray is established for the audited hand grenade path.

ShotShell (Shots.cpp:638) uses ShellParamsFor; explosionTime >0 plus enabled fuse settings selects fused behavior, with configurable fuse override. Config without fuse retains legacy behavior. BounceOff (:678) and ground/object collision paths use existing bounce/rest logic; arming/time/obstruction can move detonation away from the intended point. Safety must derive from actual loaded ammo/config and legitimate friendly awareness, include thrower and foreseeable movement, and withhold when a conservative assessment is unavailable. Do not disable all grenades or remove collisions/damage.

## Diagnostics and observer limits

Only diagnostic C++ addition: opt-in POSEIDON_COMBAT_AUDIT=1, restricted to object var name auditShooter, wraps the **one naturally occurring** GetAimed evaluation using its existing AimingDiagnostics. Logs time, requested/current mode, actual reached gate, visibility, miss/tolerance, score, knowledge age/error. No extra visibility/readiness/collision query, no RNG call, no simulation state mutation. Disabled path calls the original overload once. Logging costs are not zero.

Existing crew_pose_probe was inspected and rejected for this purpose: it targets a cargo unit and performs extra GetAimed/visibility/collision queries. Those can refresh caches and are not passive ordinary-infantry telemetry.

The new wrapper does not establish why an earlier stage never reached GetAimed; absent lines must not be labeled “visibility” or “aim failure.” It also does not record every projectile/animation event or cache timestamp. Further diagnostics, if needed, must observe natural selection/release conditions rather than recompute skipped stages.

## Existing mechanisms worth reusing

Target knowledge and visibility caches; aim/ballistic solver and shot recorder; CheckNearFriendlies broad phase and AICenter relationship rules; damage/occlusion and GrenadeFuse prediction primitives; weapon/action queues; AttackThink/EstimateAttack economical/aggressive positions (TargetFire.cpp:89/:342), center exposure fields, hide/cover and operative/strategic path planners, group/subgroup and arcade waypoint FSMs. Located mechanisms are not proof of successful autonomous flanking or coordinated suppression. This audit does not redesign tactics.

## Ranked next AI tasks

1. **Recommended one slice: autonomous on-foot rifle emission safety correctness.** Unify safety with the actual current muzzle/weapon, bound candidate collection using existing spatial data, incorporate body extent and existing dispersion, and revalidate when a burst emits after a relevant ally/weapon/muzzle change. The source-proven burst bypass and origin mismatch are stronger reasons to act than a speculative multi-aimpoint system. Cache invalidation must be measured; no unlimited all-world scan for every bullet. Keep intentional player and forced/scripted fire semantics explicit. Do not loosen any explosive or other fire gate.
2. Grenade selection plus pre-release veto: mandatory safety gap, high benefit but greater gameplay/design risk because harm area, fuse/landing uncertainty, animation cancellation and incomplete friendly knowledge need a documented contract. Existing penalty is insufficient.
3. Infantry ballistic evaluator consistency: narrow potential regression/correctness fix; promising source evidence, but range-controlled actual shot evidence must show which rejections it causes. Never “fix” by lowering tolerance gates.
4. Correct content/LOD or stale transform/cache defect if a specific cover reproduction establishes it.
5. Shooter-owned alternate aimpoint experiment only if the preferred point is established as the first real blocker. Never store observer-dependent chosen points in the shared Target or globally alter Object::AimingPosition.

Acceptance for #1: unchanged valid rifle opportunities with blockers removed/outside; player and allied other-group blockers at close and >50m ranges; occupied friendly vehicle; ally entering between burst rounds; same muzzle/solution as actual emission; existing side/neutral rules; manual/script regression controls; disabled mode unchanged RNG/state; measured bounded cost in 63MAX. Rollback: separate opt-in change, off reproduces baseline, one revertable commit; no persistence/network-format changes. Grenade safety remains an explicit unresolved obligation, not implicitly solved by rifle work.

## Runtime results

See appended measured results. Cases not actually reproduced are explicitly unresolved, not passes.

### Measured combat controls (two runs)

Diagnostic build: Combat-20260919-123951. Unchanged reference, diagnostics disabled: Combat-20260919-124706. Both use seed1234, stock SoldierWB/M16, skill1, standing shooter/target, move disabled, target and allied blocker made invulnerable **only in the test fixture** to keep repeated opportunities alive. Fired-event counts measure emissions; these are not injury/kill tests. No per-phase reseeding. Each rifle phase lasts 12 wall seconds; grenade phases 18. Times/commands/results retained in harness.jsonl.

| Case | Diagnostic / reference result | First blocking stage established |
|---|---|---|
| A, open 100m | 30 / 30 emitted rounds | No persistent block. GetAimed reaches accepted with aimed1 in successful opportunities. |
| A, open 250m | 12 / 12 emitted rounds | Some naturally observed miss scores .268/.378/.471 remain below trigger .5; later .687 permits fire. Diagnostic gate string “accepted” means the graded return path, not necessarily permission to fire. |
| A, open 400m | 0 / 0 | GetAimed not reached during phase. Earlier selection/preparation/readiness path unresolved. **Not evidence that vacuum scoring blocked the shot.** Target knowledge near .995 does not prove a valid weapon opportunity. |
| F, same-line allied other-group rifleman at 20m, enemy100m | 0 / 0 | GetAimed not reached; earlier blocker unresolved. Cannot credit CheckFriendlyFire itself for this result. |
| F, ally moved 20m sideways | 30 / 30 | Valid firing resumes. |
| G, grenade-only opportunity at30m, ally35m sideways | 0 / 0; all3 grenades retained | GetAimed not reached; fixture did not establish a viable throw. |
| G, ally at target location | 0 / 0; all3 retained | Same earlier block. **Not a demonstrated safety pass**, because the safe throw control also did not release. |

The last two tests do not demonstrate an unsafe grenade release. Source control-flow/cost defects remain independently confirmed. A crossing during an actual grenade animation or rifle burst, player/occupied-vehicle blockers and a high-value mixed cluster remain untested.

Observer check: after discarding the first ten reporting windows, 97 windows each; tick-weighted simulation means **.139ms with logging vs .138ms reference**, 46 audit log lines vs0. Same emission counts, but one ordered pair is not a statistical no-overhead guarantee and does not prove identical RNG trajectories. No repeated query/cache refresh was added.

B object partial exposure, C comparable terrain exposure, D never-known/previously-known hidden, E eye-clear/muzzle-blocked **were not runtime established**. Their first failed gates are unresolved. Do not present the source hypotheses as reproductions. Next bounded fixture extension should use a known collision-tested opaque wall, first prove open-fire control, then lower the wall/move target so the preferred point is blocked with visible body extent; use the same dimensions to arrange a terrain edge and an eye/muzzle-height obstruction. Observe natural acquisition/selection gates before adding aimpoint sampling. If no GetAimed call occurs, extend passive selection diagnostics first rather than manually calling visibility/GetAimed to populate a table.

### Helikopter

The original test had two independent fixture defects: array passed to the object-only distance command (runtime script abort), and teleporting a cold, empty aircraft into the air after inserting a pilot (own first repaired-script run crashed). Neither is accepted as a helicopter flight/motion test.

Fixed fixture uses a crewed WEST UH60 with mission special=FLY, initialized by MakeAirborne (AICenterImpl.cpp:1134); repeated east/west doMove, no artificial per-frame repositioning. Endpoint checks use getPos X instead of invalid object-to-array distance. No automatic mission end.

Helicopter-20260919-123525: 90s sampled flight, simulation time ~10–102s, pilot alive throughout, several route reversals, altitude around80–92m in recorded samples. Screenshots inspected. After first10 reporting windows: 91 windows /5494 ticks, **.102ms simulation/tick, zero catch-up discards**. This validates the mission and rules out sustained simulation overload in that run, not every presentation hitch.

Owner observed mostly fluid movement with occasional apparent skipped frame/jump. **Open issue**, not marked fixed. Inspect presentation/frame pacing, camera tracking versus Object::RenderTransform interpolation and helicopter CPU-pose submission separately. EngineWgpu.cpp:5198 “producer window opened late by helicopter CPU pose” is a synchronization diagnostic, **not proof of a stale or dropped pose**. Object.cpp:86 interpolates body history, can snap for invalid history/large jumps; World.cpp:988 captures before each fixed tick. No pose/presentation production change was made.

The new runner additionally supports -FixedCamera and a bounded triPerfCapture interval, with the screenshot taken before capture. The manual Start-AIDistanceTest.ps1 in THIS worktree uses the corrected unlimited-duration mission. The old farvisible mission was intentionally not overwritten.

Fixed-camera follow-up Helicopter-20260919-124925 captured 1783 profiler records in30s, no capture overflow: mean CPU work .965ms, p95 1.575ms, p99 2.243ms, maximum4.956ms. Largest recorded swap work3.707ms; sim-step maximum .791ms. **These are profiled CPU work durations, not display/present-to-present intervals**; they do not exclude an FPS-limiter/OS/display hitch. No expensive simulation/CPU-render-work spike was reproduced in this bounded interval. Owner-reported occasional visual skip remains unresolved; no claim of a pose fix.

### 63MAX stress / remaining performance scope

Actual supplied mission path is Mods/@63units/63MAX.west_ls2. Reference binary, same settings/seed/content,50s sampling after15s harness warm-up. Comparison excludes first10 per-second reporting windows; means weighted by each window's tick count.

| Run | Windows/ticks | Total sim ms/tick | AI | Vehicles | Visibility | Discarded steps |
|---|---:|---:|---:|---:|---:|---:|
| Stress-20260919-124209, detailed vehicle cost enabled |49 /2817|16.648|4.359|10.527|1.662|294|
| Stress-20260919-124342, detailed cost disabled |49 /2951|14.402|4.356|8.478|1.469|98|

The detailed probe materially perturbs this workload (~2.25ms total difference in this ordered pair); **do not claim its16.65ms is the uninstrumented engine cost**. AI time stayed similar. Game evolution/pacing differs between runs, so this is not a perfect deterministic matched frame comparison.

Detailed samples show ~708 man Simulate calls/tick and ~26,000–27,000 “other” calls/tick; individual windows: slow.moveout ~1.8–2.2ms, slow.animals ~3.2–3.6ms, man base ~3.0–3.6ms. “Other” is a category, not a measured all-footprint count. Footprint marks are a strong candidate given existing PERF-032 evidence and Mark's shared lists, but require type attribution before removing/changing behavior. Do not disable footprints or lower AI rate and call that a transparent optimization.

Uninstrumented GPU frame samples also reached **17.523,23.353,23.602ms** late in the run. The stress problem includes rendering throughput in addition to simulation/list maintenance. No vanilla binary was run, and no numeric vanilla speed ratio is claimed.

Useful next performance work: profile temporary-object list traversal/cleanup and Mark updates without expensive per-object type classification; preserve fade/deletion and collision-query semantics. Separately identify expensive GPU water/effect work for this camera before making a quality trade. The existing target membership optimization is retained. No new performance speedup is claimed by this audit. These are independent performance candidates, not a second selected AI gameplay patch.

### Files and validation

C++: SoldierOldAI.cpp (opt-in passive wrapper only).
Test files: combat_audit.Intro/{mission.sqm,init.sqs}; corrected perf_heli_distance.Intro/{mission.sqm,init.sqs}; scripts/Run-CombatAudit.ps1. Report and handover ledger on this branch. PowerShell parsed; PoseidonGame build succeeded; successful reference/diagnostic runtime runs exit0 without script/WGPU validation errors. No broad CTest/smoke campaign was added.

Initial fixture failures are retained in build/audit-runs and labeled above, not counted as passes. Two earlier scripts emitted a provenance JSON depth warning for redundant nested PowerShell provider metadata; core names/values, args and hashes are present. The final AI runner flattens environment fields and additionally hashes loaded CONFIG.BIN/remaster.cpp/mission.sqm.

Post-run installed executable/dll hashes remain 06F8259C3089F6A8FAF95A5FEA9229FFE927567FDC621E00D9F04BA309D9C8C2 / 0B609A4E3910B44992EE782B090C7BDC03C8130EBB9BB76D1640C31FBBA189E5; CONFIG.BIN hash DA9B790900357450D06CB27FD0CDBCF671E6316D6A6D4D0C9982D358AFF703CA. No normal-install deployment, merge, multiplayer or save/load test. Audit stops before production changes as requested.
