# Dense infantry progress and distance hitches — 2026-09-19

Owner authorized investigating 63MAX near/far performance, stationary soldiers,
and unexplained skyward shots after accepting crossfire fix 7c3d528a.
Installed reference: codex/combat-fixes 7c3d528a, matching EXE/DLL; implementation
continues in the isolated combat-audit worktree and is carried onto
parity/foliage-lighting-001. No foreign working files are modified.

## Distance reassignment

World::DistributeImportances removed each transferred element with Delete(i),
shifting all later references each time. Camera distance changes can transfer
thousands of footprint marks between simulation groups; 63MAX produced roughly
26,000 marks in the first reference run. The routine was quadratic in this case.
StableExtract preserves the order in source and destination, acquires destination
ownership before compacting, then shrinks once. Simulation rates, entity counts,
visibility thresholds and per-entity decisions are unchanged.

Ownership/order tests transfer 20,000 reference-counted objects, retain stable
ordering, visit each once, preserve all lifetimes until the destination releases
them, and cover empty/no-transfer/all-transfer groups.

Reference StressDistance-20260919-150116: camera phases begin at mission times
10.302 / 18.292 / 24.259; worst importance windows total **747.018 and 762.390 ms**
(window tick count multiplied by its reported ms/tick). These are accumulated
reassignment work, not directly measured individual frame intervals. CPU frame
profiler sim:step max is 910.400 ms for the complete capture. The scope includes
per-entity diagnostic timing, which adds overhead to the tens of thousands of
marks; it must not be represented as uninstrumented FPS or vanilla parity.

The first changed run (StressDistance-20260919-151312) started at mission time
0.612 rather than 10.302 because startup wall time varied. It is not accepted as
a matched performance comparison. The driver now waits for mission time 10.25
before the same far/near/far camera sequence, with the player unchanged.

## Blocked firing pauses

AIPilot held FireAim/FireAimed (zero movement wanted) for up to 10–20 seconds even
when already-scheduled friendly-fire checks continuously rejected the shot.
The improved local autonomous infantry path now observes these existing
rejections. After at least 0.75 seconds of recent rejection for the same target,
an active unit with an existing route more than two metres long can release its
firing pause for two seconds and resume the normal route/formation pilot.
It invents no sidestep/teleport, performs no additional visibility query and
never relaxes the per-round friendly-fire veto. Stopped/stopping units,
disableAI MOVE, external animation control, manual, scripted force-fire, mounted
and multiplayer paths retain their contracts. The pending observation resets
on load, target change, successful emission, stale samples and clock rewind.
The existing AI-tab Improved/Legacy setting controls this change.

## Burst aim eligibility

SimulateWeaponActivity emitted remaining burst rounds through FireWeapon without
AIFire's GetAimed check. A forced turn after round one reproduced two off-heading
rounds in the installed reference (BurstAim-20260919-151524: nine total rounds,
two counted while body heading was outside +/-30 degrees, then re-aimed).
The improved bullet continuation reuses GetAimed >= 0.5 before each loaded burst
round. No accuracy/skill threshold changes. A missing/destroyed/changed target
cancels only the un-emitted remainder, consuming no ammunition. Actual emitted
projectiles are unaffected. This establishes a burst direction-loss defect;
it does not by itself prove every owner-observed upward shot has this cause.

## Diagnostics and validation

ai_stress_state reads current movement wanted, actual speed, fire pause, unit
state, route size, recent lane rejection and yield count without recomputing AI
decisions. Optional recordShots enables the existing bounded ballistic recorder
(128 tracks, no trail drawing), exposing sampled projectile direction and ammo.
Snapshotting the barrel alone cannot establish the direction of an emitted shot.

Automated simulation-distribution, combat, ballistics, grenade and interpolation
suite: **46 cases / 20,503 assertions passed** before final runtime acceptance.
All game runs use with-game-lock.sh and isolated user configuration. Runtime
results and installed payload identity are appended after completion.

## Runtime results before installation

- Matched follow-up StressDistance-20260919-151859 begins at mission times
  10.948 / 19.261 / 25.806. Worst importance window **2.160 ms**, versus 762.390 ms
  reference. Maximum non-actor calls/tick are 26,468.4 versus 26,402.4, so this
  improvement is not obtained by deleting marks or testing a substantially
  smaller population. CPU profiler sim:step average 61.062 ms, p95 171.494 ms,
  maximum 290.216 ms, versus reference 100.870 / 189.288 / 910.400 ms. These are
  one instrumented paired sequence with slightly different progression, not a
  statistical/uninstrumented FPS claim. Steady dense-scene overload remains.
- BurstAim-20260919-152426: same-process Legacy emits nine rounds including two
  counted off-heading; Improved emits only the first during the forced-turn
  interval, with zero off-heading rounds. Subsequent free-lane firing is covered
  by the separate crossfire control.
- LaneMove-20260919-152045 exposed an invalid fixture use of enableAI (unsupported
  script command); it is not a product acceptance. The new combat_lane.Intro
  starts movement-enabled and uses existing doMove/doStop commands.
- LaneMove-20260919-152328 did not establish a timed yield: ordinary aim sway
  repeatedly cleared the observation. The implementation was corrected to retain
  recent observed blocking through other aim gates; an absence of another block
  for 0.75 s still expires it, as do target change/clock rewind/successful emission.
- Final LaneMove-20260919-152803: Legacy remains at z=3606.85 from mission time
  15.334 to 24.004 with zero rounds. Improved records actual laneYields=1 at
  31.552 and laneYields=2 at 38.046, resumes the existing route, safely passes
  the stationary friend at z=3620, reaches z=3630.27 and fires three rounds once
  clear. This is causal yield telemetry, not merely a difference in final position.

- StressBehavior-20260919-152955: six post-centre snapshots from mission time
  10.268 to 23.324 retain 707 living infantry. Observed lane yields increase to
  21; 280 units are still moving in the last sample. Player fire-target holders
  increase to 265, and 165 units have a recent blocked-lane observation. The
  ballistic sampler retains three BulletBurstE tracks; maximum sampled dirY is
  -0.00992. No upward shot was captured, but three sampled tracks (short-lived
  projectiles can disappear between samples) cannot exclude every intermittent
  sky-shot. The reproduced burst direction-loss gap is the accepted correction.

- Crossfire-20260919-153143 rechecks the accepted safety fixture after all new
  combat changes: **0 / 0 / 0 / 21** rounds. Friends at 20/150/300 m inhibit
  fire; 20 m lateral clearance restores the original 21-round control. Normal
  clear-lane burst volume is preserved in this fixture. Clean harness exit.

## Installed delivery and helicopter regression

Production commit **a30452b2** was built from the committed tree, pushed to the
safety remote, fast-forwarded onto parity/foliage-lighting-001, and deployed as a
matching EXE/DLL pair on 2026-09-19 at 15:37 local time. Installed and staged hashes:

- OpenPoseidon.exe SHA256: 9E801F7EE1DF305E621A3FD7D39768600A0984BF3BE4654943F5EA41592361F3
- wgpu_renderer.dll SHA256: 98E175EA7D14BF4008CA7AB473148BBF3B53F0A4462050A129B637B312DF87E2

Helicopter-20260919-153754 runs that installed pair via the isolated reference
copy, fixed camera, 30-second observation. Pilot remains alive throughout and
harness exits cleanly. FramePaceTrace reports no dropped trace records. After
20 seconds startup, 1,589 main-loop deltaT samples average 17.219 ms, p99 17.523 ms,
maximum 300 ms (the simulation delta clamp). The outlier must not be discarded:
frame 1403 spends 306.971 ms in drawing; CPU profiler identifies land:gnd maximum
306.848 ms. The following frame catches up eight steps and pacing diagnostics
report ten discarded steps. Thus occasional helicopter presentation jumps are
NOT closed by this result. The trace localizes this captured stall to the terrain
render phase, not expensive helicopter simulation (capture sim:step max 0.749 ms),
but does not yet establish the lower-level cause. Startup separately discarded
20 steps across two frames. No additional helicopter code was changed here.

Raw artifacts: build/audit-runs/Helicopter-20260919-153754 and
build/heli-after-ai-progress.{main,producer,worker}.csv. The remaining rendering
stall and dense-scene overload remain open, as does broader owner visual acceptance.
The infantry improvements default to Improved and retain the AI-tab Legacy switch.
