# PERF-033 - Tiered visible-far simulation rate

Date: 2026-09-14

Status: implemented, measured and owner smoke-tested on
`fix/far-visible-smooth-001` from baseline `asset-obs-001` (`7d871e92`).
Commit `8274497b`, installed and verified in the game folder
(stamp `fix/far-visible-smooth-001 8274497b`).

## Problem

PERF-032 routed `VisibleFar` through `SimulateOptimized`, which honours the
declared `SimulationPrecision`. Soldiers declare 1/5 s
(`SoldierOld.cpp`), so distant-visible troops froze for ~11 ticks and then
jumped 0.2 s at once. There is no skeletal interpolation to hide it
(`Man::Animate` reads `_primaryTime` directly), so far AI read as ~5 fps
while everything else ran at 60.

The first fix attempt (`87fa6cb7`) put `VisibleFar` back on the full-rate
`SimulateRest` path. The owner measured a full regression on the
`@63units/63MAX.west_ls2` mission (63 groups, 708 soldiers): low frame rate
again. This matches the PERF-032 control arm (26-50 ms/tick full-rate), so
full-rate visible-far is not a viable fallback.

## Implementation

Two files, no header changes, no save-format change:

- `engine/Poseidon/World/Simulation/Simul.cpp`:
  `Entity::SimulateOptimized` caps its accumulator step to
  `POSEIDON_FAR_VISIBLE_STEP` (seconds, clamped to (0, 1], default 1/15)
  **only** when `prec == SimulateVisibleFar`. Smaller steps run more often
  but simulate the same total time, so behaviour converges toward full-rate
  instead of away from it. No `Tank` override change needed: tanks keep the
  default 1/15 precision, so the cap is a no-op for them. Footprint marks
  (0.5 s precision) are also capped in the visible-far bucket, which saves
  additional cost on top of the PERF-032 mirrors.
- `engine/Poseidon/World/WorldSetup.cpp`: `VisibleFar` is routed through
  `farSimul` again (as in PERF-032); the smoothness now comes from the cap,
  not from full-rate simulation. `POSEIDON_FAR_SIM_FULL_RATE=1` still
  restores the old path for A/B comparison.

Resulting tiers: near = 60 Hz, visible-far ~= 15 Hz (tunable without a
rebuild), invisible = declared precision (soldiers 5 Hz).

## Measurements

Same mission, WGPU, 1280x720 window, `--dev`, installed matched EXE/DLL,
per-second sim log (`Sim stage`, `Sim pacing`, `Sim vehicles`):

| Arm | Sim total ms/tick (avg / max) | slow.vehicles (avg) | AI groupScan (avg) | Pacing |
|---|---|---|---|---|
| default cap 1/15 | 15.2 / 20.1 over 63 windows | 6.2 | 4.0 | ~60 ticks/s, intermittent discards (0-35 per window, mostly single digits) |
| `POSEIDON_FAR_VISIBLE_STEP=0.1` | 18.0 / 27.3 over 68 windows | 6.2 | - | discards in nearly every window |

Two findings:

1. Lowering the visible-far rate to 10 Hz does **not** reduce body cost
   (`slow.vehicles` identical at 6.2 ms) and the run was worse overall.
   Mission state diverges between runs (live battle), so single-run
   comparisons bounce; but the lever is confirmed weak. Do not chase this
   direction.
2. The remaining budget pressure is AI think (~4 ms `groupScan`, driven by
   `expensive` subgroup work over 708 targets) plus list maintenance
   (`slow.moveout` ~1.1 ms, `slow.animals` ~0.6 ms) - not the visible-far
   rate. Those belong to follow-ups (AI scheduling) outside this change.

Owner verdict on the 1/15 default: "runs almost good"; framerate not
perfectly fluid, which is plausible as normal for this extreme scene
(sim average 15.2 of 16.7 ms leaves little headroom, and 708 drawn
soldiers are render-heavy on their own).

## Verification

- `win-x64-clang-rwdi` configure clean, `PoseidonGame` target builds
  with no new warnings (only pre-existing deprecation/overlay notes).
- Deployed only via `scripts/Deploy.ps1` under the game lock; stamp and
  matched EXE+DLL timestamps confirmed.
- Launch check: 45 s windowed run, `Wgpu: creating renderer ... build
  8274497b`, no panic, no exception, render thread producing frames.

## Risks and remaining acceptance

- Same class as PERF-032: far-bucket step sizes affect integration paths.
  Multiplayer authority, save/load and deterministic replay comparison are
  still open before touching precision values themselves; this change does
  not touch them.
- Heavy combat moments still discard steps on this mission. If that needs
  to go to zero, the next lever is AI think cost, not the visible-far rate.
- Merge note: this branch is based directly on `asset-obs-001`
  (`7d871e92`), two commits ahead of it. Merging into `asset-obs-001` is
  `git merge safety/fix/far-visible-smooth-001` with no expected conflicts
  (only `Simul.cpp` and `WorldSetup.cpp` touched; `Cargo.lock` churn from
  local builds was deliberately left uncommitted).
