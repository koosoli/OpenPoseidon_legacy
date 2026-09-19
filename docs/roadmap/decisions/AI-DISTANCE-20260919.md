# Distant helicopters and dense AI target discovery

Branch: `parity/foliage-lighting-001`, baseline `4a2a30b4`.
Implementation: `4ac8199c`. Owner runtime and performance acceptance pending.

## PERF-034: helicopter cadence

`Helicopter` declares 1/15 s precision. PERF-033's cap therefore does not
increase its update rate. `World::CaptureRenderPrevFrames` captures every
1/60 s tick: three ticks retain the same pose, then one blends a four-tick
movement over one tick. This explains a stepping mechanism, not a measured
four-FPS helicopter rate. Rotor animation also advances during simulation.

`Helicopter::SimulateOptimized` now uses the existing `SimulateRest` path for
VisibleFar. This consumes accumulated time and applies remote state using
the existing mechanism; no additional time is simulated. Other buckets retain
the base implementation. Infantry/tank cadence and global fixed-step timing
are unchanged. The camera and nearby helicopter already used SimulateRest.

`POSEIDON_HELI_FAR_FULL_RATE=0` restores the previous behavior independently
of infantry. `WorstImportance()` clamps helicopters to VisibleFar even beyond
view distance: many off-screen helicopters will also cost more. This is a
deliberate smoothness candidate, not a helicopter-heavy CPU optimization.
Flight trajectories, combat, rotor appearance, save/load and MP still require
runtime acceptance. This is not render-only interpolation.

## PERF-035: target membership during discovery

`EntityAI::AddNewTargets` previously linearly searched the group's entire
target list for every candidate passing its range/type/landscape filters.
With V candidates and T known targets this is O(V*T) identity comparisons
per sensor scan, in addition to visibility work elsewhere.

A local pointer set is built from the current target list. Existing candidates
are tested against it and new candidates inserted as they are appended to the
authoritative list. Expected membership work becomes O(V+T). World traversal,
append order, target initializers, nearest-enemy calculation and scan intervals
stay unchanged; the hash table is never iterated to make a gameplay decision.
It is discarded after the call, so no cache invalidation or save schema is added.
Allocation/hashing may cost more for tiny scenes; no game FPS gain is claimed.

`POSEIDON_TARGET_SCAN_LINEAR=1` restores the original linear membership lookup
using `TargetList::Find`, in the same binary, for owner comparisons.

## Checks and owner test

- WGPU RelWithDebInfo game and PoseidonTests build.
- Existing `[ai][determinism],[helicopter]` selection: 248 assertions / 9 cases.
  These cover existing AI ordering and a helicopter header check; they do not
  validate actual flight, the new membership scan against a live world, or MP.
- No game process was launched by the assistant, as explicitly requested.
- `scripts/Start-AIDistanceTest.ps1`: owner-operated, holds the game-folder
  lock, writes timestamped logs, restores process environment after exit.
- Default scenario: `tests/perf/missions/perf_heli_distance.Intro`, observer
  looks north toward a UH-60 crossing roughly 550 m away at 80 m altitude.
  The scenario itself remains owner-unverified. No automated capture/exit.

From this worktree:

```powershell
.\scripts\Start-AIDistanceTest.ps1
# Optional helicopter control, after closing the first run:
.\scripts\Start-AIDistanceTest.ps1 -LegacyHelicopter
# Existing 63-group mission:
.\scripts\Start-AIDistanceTest.ps1 -Scenario Stress
# Optional target-search control, same settings and camera:
.\scripts\Start-AIDistanceTest.ps1 -Scenario Stress -LegacyTargetScan
```

Compare helicopter body travel and bank/turn motion as well as rotor appearance.
For the stress mission compare the same initial camera/time window, including
`Sim stage`, AI `groupScan` and discarded fixed steps in the logs. Combat can
diverge across runs, so a single FPS endpoint is not a controlled speedup.
Logs/build evidence: `build/ai-distance*` (local, not committed).

The full COMPAT-C1 ledger validator stops at a pre-existing AST-011B evidence
hash mismatch (recorded 2d228694..., actual ce123a20...). None of its twelve
evidence files were changed by this task. Historical evidence was not restamped.
