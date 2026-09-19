# Combat implementation follow-up — 2026-09-19

The owner explicitly authorized implementation after COMBAT-AUDIT-20260919.md,
including self-run smoke tests and an improved/legacy comparison in the existing
AI tab. Work continues from parity/foliage-lighting-001, not another trunk.
Implementation worktree: combat-audit, branch codex/combat-fixes.

## Changes

- `Dev tools > AI > Combat behaviour`: Improved / Legacy (before combat fixes).
  Session-local, immediately effective, with POSEIDON_AI_COMBAT_LEGACY=1 as the
  startup control. Legacy does not mean every engine subsystem becomes vanilla.
  Target-scan/watch sliders and the Ballistics tab remain independent.
- New combat behaviour applies to local, autonomous, on-foot, singleplayer AI.
  Player/manual control, mounted infantry, multiplayer and forced-fire bypass it.
- Bullet shot evaluation uses the existing drag-aware solver's time and drop.
  Muzzle visibility uses the evaluated weapon instead of an unrelated selected
  slot. Knowledge, visibility thresholds, skill and RNG consumption are retained.
- Every actual autonomous FireWeapon entry, including burst continuation,
  rechecks current firing permission and friendly bodies around the actual muzzle
  direction. The existing allegiance rules apply to other groups and occupied
  vehicles as well. Queries use the landscape's actor-only spatial index, not the
  footprint population. Body extent and authored weapon dispersion expand the
  corridor; it continues beyond the enemy to the authored range (maximum 1 km).
- Hand-grenade selection has a hard allied-area veto independent of enemy value.
  Animation start and the actual pre-emission ThrowGrenadeAction both revalidate.
  The blast broad phase comes from 4*indirectHitRange plus body/landing uncertainty.
  Bounded flight prediction reuses drag parameters and GrenadeFuse::Bounce for
  terrain contact. Fused throws obstructed by objects are withheld. No emitted
  grenade is cancelled; no ammunition is consumed on a release-time veto.
- The shot-effect change remains a separate rendering commit. Infantry MGunClouds
  opt out of smoke shadows; the weapon light is a separate light source and is
  unchanged. Intentional SmokeShell sources keep their shadow participation.
- AppIdle now feeds simulation/interpolation from the microsecond clock already
  used by the pacer, eliminating integer-ms quantization of delivered delta time.
  Fixed/lockstep overrides and the existing .3-second clamp remain intact.
- MoveOutAndDelete avoids incrementing/decrementing a temporary reference for
  ordinary survivors. Lifecycle callbacks retain their guard and stable order.

## Verification

Build: scripts/Build.ps1, win-x64-clang-rwdi, PoseidonGame and PoseidonTests.
Targeted existing ballistics, grenade-response and render-interpolation tests:
**39 cases, 429 assertions passed** (`build/combat-unit-tests.log`).

All game runs use with-game-lock.sh and an isolated user/configuration directory.
Run provenance includes executable/DLL/data hashes and the installed stamp.
Early runs use the recorded working-copy implementation; the committed production
code is cf18101f (AI d2f82018, renderer 6992a839 following 962bebac).

| Run | Observation |
| --- | --- |
| Combat-20260919-131628 | 100/250/400 m: 30/12/0 rounds; friend inline/clear: 0/30. Matches prior control. 400 m exceeds the installed M16 AI envelopes, so it is not evidence of a new aiming defect. |
| Grenades-20260919-132143 | Passive config probe established HandGrenade min/max AI range 40/60 m, blast range 7 m. The earlier 30 m fixture could never validate grenade safety. Runtime switch changed both configured and effective state. |
| Grenades-20260919-132255 | Corrected 45 m control: 3 releases with friend 50 m aside; 0 with friend at the target. Subsequent fixture revision also restores target position and adds a clear-again phase. |
| Burst-20260919-133418 | Same process, actual AI-tab setting backend: an event inserts an allied other-group soldier 10 m ahead immediately after round one. Legacy emits all 3 burst rounds; Improved emits only the already-authorized first round. No subsequent rounds after ally entry. This verifies the emission-path bypass and its fix, not immunity of a round already airborne. |
| Shadows-20260919-132406 | Ordinary muzzle cloudlets 11/12 with zero shadow blobs; intentional smoke 122 cloudlets/108 blobs, later 457/439 and the 512 cap. Rifle screenshot inspected: sprite still visible, soldier shadow retained. Previous reference rifle had nonzero smoke-shadow blobs. |
| Helicopter-20260919-131214 | Before microsecond delta change: 1,638 post-warmup rows, deltaT average 17.100 ms, p99 18 ms, max 28 ms; all delta inputs rounded to whole ms. This is a real frame trace, unlike the earlier CPU-work-only profiler capture. |
| Helicopter-20260919-132900 | After clock correction: 1,636 post-warmup rows, average 17.084 ms, p99 17.457 ms, max 21.090 ms. Only 9 rows round to whole ms at CSV precision, vs all 1,638 before. Maxima are observations from short runs, not a guarantee against presentation stalls. |
| Stress-20260919-133039 / Stress-20260919-133250 | Same first 45 timing windows, omit first 10; weighted 2,144 / 2,130 ticks. Simulation 13.184 -> 12.694 ms/tick; AI 4.073 -> 3.993; vehicles 7.306 -> 6.964; visibility 1.713 -> 1.634. Mean moveout pass 1.581 -> 1.082 ms. A modest paired-run gain, not statistical proof of universal FPS uplift. |

The reference stress process shut down cleanly with exit code zero during the
final screenshot request, so its harness reported a disconnected socket. The
preceding telemetry is intact; no successful final screenshot/driver completion
is claimed for that run. The fixed 35-second measurement completed normally.
Whole runs recorded 45 / 55 discarded steps, with different durations; these
counts do not establish an improvement in worst-case hitching.

## Limits of the claim

This is a bounded infantry improvement, not a general guarantee against every
ballistic accident. The bullet corridor is not a ricochet/penetration simulator
and does not follow all curved residual travel beyond the authored range. The
grenade predictor is approximate; custom fuses beyond eight seconds/unsupported
initial delays fail closed, and object-obstructed fused throws are withheld.
Friendly movement expansion uses current speed and a bounded spatial horizon;
unpredictable motion after emission is not retroactively prevented.

The smoke runtime control exercises the ordinary ground-shadow collector;
volumetric exclusion follows the same SampleShadow flag in source but is not a
separate volumetric runtime acceptance. Partial cover, unseen target memory,
custom fused/bouncing grenades, save/load and multiplayer are not newly accepted
by the open-field cases. No global thresholds, hidden-target pose queries or
multi-aimpoint targeting were added. No comparison to a newly measured vanilla
executable is claimed. Occasional presentation hitches still require visual
acceptance; a finer simulation clock does not eliminate driver/GPU stalls.
