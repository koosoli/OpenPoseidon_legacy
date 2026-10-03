# DIAG-01 — W1: the Malprave diagnostics layer (`--diag` event log, `diag_*` harness commands)

Branch: `dec/diag-w1-malprave-diagnostics`, on top of `dec/tidewater-w12e-docs-showcase` (95a9eb1d).
Ledger: `DIAG-001`. Status: integrated, unit-tested, builds with the option ON and OFF; **not yet run in game**.

## Why

Malprave (Dec's GPL fork of the same Poseidon source, no shared git history) grew a test and diagnostics
layer — `mwDiag` — over the Cry of Fear port: a JSON-lines event log of what the engine did during a
scripted test run, harness commands to inspect units, pause and step the world and pin the camera, and a
PowerShell runner that plays a step list and writes one summary. Most of it is not about Cry of Fear at all.
The owner asked for the generic part in OP so OP test runs can answer "what happened" from a file instead of
from screenshots alone.

## What was ported, from where

All new code is in `engine/Poseidon/Dev/Diag/OpDiag*` (namespace `Poseidon::Dev::OpDiag`), compiled only
with `POSEIDON_DIAG=1`.

| OP file | From Malprave `engine/Poseidon/Dev/Diag/` | Content |
| --- | --- | --- |
| `OpDiag.hpp`, `OpDiag.cpp` | `MwDiag.hpp`, `MwDiag.cpp` | event writer (`Ev`), lifetime, categories, boot / addon / script / sound / death / move / shot / impact / shot_end / hit / frame_slow / perf / mark events, damage bracket, watched units, overlay store, time control |
| `OpDiagInternal.hpp` | `MwDiagInternal.hpp` | shared internals |
| `OpDiagJson.hpp` | `MwDiag.cpp` (`AppendJsonString`, `AppendNum`, `AppendVec`), `MwDiagHarness.cpp` (`J`) | header-only JSON text helpers, unit-tested |
| `OpDiagStuck.hpp` | `MwDiag2.cpp` `PollUnits` | the stuck / spin detector as pure logic, unit-tested |
| `OpDiagPollers.cpp` | `MwDiag2.cpp`, `MwDiag7.cpp` | AI state of watched units, stuck / spin, corpses, trails, sound_play / sound_stats, crash event, `--diag-seed`, `--diag-fixed-dt` |
| `OpDiagHarness.cpp` | `MwDiagHarness.cpp`, `MwDiagHarness2.cpp` | the `diag_*` commands below; `Man::DiagMoveName / DiagInspect / DiagAnimInfo / DiagSetPose` |
| `OpDiagCamera.cpp` | `MwDiag9.cpp` | `diag_camera` |
| `OpDiagOverlay.cpp` | `MwDiagOverlay.cpp` | the 3D overlay (`--diag-draw`) |

Engine hooks (the ~25 `#if MALPRAVE_DIAG` blocks of Malprave, now `#if POSEIDON_DIAG`): `World.cpp`
(fixed-step tick for `diag_step`, end-of-frame `OnFrameEnd`), `WorldImpl.cpp` (test-mission boot result),
`Shots.cpp` (shell / grenade / missile flight path, fuse and timeout ends), `Collisions.cpp`
(`Landscape::ExplosionDammage` impact), `Dammage.cpp` (`hit` bracket around `DoDammage`), `VehicleAICombat.cpp`
(`EntityAI::Destroy` → `death`), `SoldierOldMove.cpp` (`move` on a primary-move change), `ExpressExt.cpp`
(script errors, `--diag-keep-going`), `SoundScene.cpp` (`sound_play`, open failures), `VehicleTypes.cpp`
(addon class not activated), `OptionsUI.cpp` (CfgSounds miss), `DebugOverlay.cpp` (overlay draw),
`GameApplication.cpp` (init, shutdown, harness registration), `AppConfig` (flags), `CrashHandler`
(`SetCrashNotify`, Windows; a no-op on Linux). `Man`, `Script` and `World` get a few accessors under the same
macro.

## What was rewritten for this tree

* **Pause and step drive Oli's dev pause** (`Dev/Diag/DiagPause.hpp`) instead of Malprave's
  `World::SetSimulationEnabled` pause. `diag_pause` / `diag_resume` call `SetDiagPause`, so they show in the
  dev panel the same way and are refused in multiplayer the same way. `diag_step {ticks}` lets single
  **fixed-step ticks** through the pause: `World::Simulate` asks `BeginStepTick()` before the fixed-step loop,
  runs exactly one tick that frame, and `EndStepTick()` re-engages the pause before anything else in the frame
  asks. Malprave's `dt` argument is gone: OP's tick length is the accumulator's (1/60 s).
* **`--diag-fixed-dt` is not a second fixed-step mechanism.** It sets `POSEIDON_LOCKSTEP_HZ = 1/dt` (SIM-815's
  lockstep pacing, `Core/Game/GameLoop.cpp`) before the world reads it, unless it is already set; `OpDiag::Init`
  runs before `InitializeWorld` for that reason, and logs a warning if lockstep had already been read.
* **`--diag-seed`** seeds `GRandGen` once the test mission has booted (after `WorldInit`'s own seeding, which
  is 1337 under lockstep).
* **`diag_camera`** uses `World_SetTriViewOverride` / `World_ClearTriViewOverride`, the same override as the
  `triSetView` test verb. It follows with or without `--diag` (ticked from the end-of-frame hook).
* **The overlay** projects the way the dev panel's Zeus markers do (`GScene->ScaledInvTransform()`, the camera
  projection, framebuffer pixels scaled to ImGui's display) and draws on ImGui's foreground list inside
  `DebugOverlay::Render`, before `ImGui::Render`. That path composites over both the wgpu and the GL33 frame,
  so it fitted without renderer changes. It needs the dev overlay (non-release build, `--dev`, the default).
* **Frame events** use `Dev::FrameProfiler`'s phases and names (`sim:step` etc.); OP has no
  `gPerfLandDrawCalls` / `gPerfSmallCulled`, so those fields are gone.
* **`move` events** come from a side table in `OpDiag` (last primary move per watched man), not from the Cry of
  Fear melee state Malprave piggy-backed on.

## What was stripped (Cry of Fear only)

Undead health pool in `hit`, the `dmg` / `hitzone` / `stagger` / `melee` / `melee_hit` / `melee_miss` /
`melee_stuck` / `chase` / `walls` / `footprint` / `anim_jump` / `step` / `shot_aim` / `sndev` events,
`diag_melee`, `diag_hitzone`, `diag_explode`, the undead / stagger / melee parts of the `Man` inspector, the
whole of `diag_config_check` (its remaining checks were melee / undead sound config).

## Left out (generic, but not this round)

* **X-ray** (`diag_xray`, and `diag_camera {clear}`): hiding objects needs a skip in the scene draw, and OP's
  wgpu object path (retained meshes, far placements) is not the single `Scene` draw call Malprave hooked;
  doing it properly is its own change.
* `diag_render`, `diag_model`, `diag_skeleton`, `diag_vdsweep`, `diag_config_check`.
* Per-frame file-load lists in `frame_slow`, per-group think cost in `diag_groups` / `group_slow`,
  `anim_load` / `anim_missing` / `skeleton` events (each needs one more hook; OP already has
  `AICost` and the streaming diagnostics for most of it).

## How to use

Flags (registered only when `POSEIDON_DIAG` and not a release-tagged build; listed under `--help --dev`):

| Flag | Meaning |
| --- | --- |
| `--diag <dir>` | write `<dir>/events.jsonl` (one JSON object per line, flushed per line) and `<dir>/summary.json` at exit or crash |
| `--diag-cats a,b,c` | event types to write: `boot,addon,script,sound,shot,hit,death,move,moveall,frame,perf,ai,stuck,trail` (default all but `moveall`; `start`, `mark`, `exit`, `crash` are always written) |
| `--diag-draw` | draw shot paths, impacts, damage numbers, STUCK / SPIN / DEAD labels and watched units (label, facing, 10 s trail, AI path) |
| `--diag-keep-going` | in `--test-mission` mode, log script errors instead of ending the mission |
| `--diag-slow-ms <ms>` | frame time that counts as slow (default 40) |
| `--diag-seed <n>` | seed `GRandGen` with n once the test mission has booted |
| `--diag-fixed-dt <s>` | pin the frame delta: sets `POSEIDON_LOCKSTEP_HZ=1/s` unless already set |
| `--diag-stuck-sec <s>` | seconds an AI unit may stand still with somewhere to go before `stuck` (default 5) |

Every event line has `ev`, `t` (mission s), `rt` (real s since start), `f` (frame). The field lists are at the
top of `OpDiag.cpp` and `OpDiagPollers.cpp`.

Harness commands (need `--harness`; they work without `--diag`, events need it). A `unit` argument is SQF that
evaluates to an object (`"s0"`, `"player"`, `"nearestObject [...]"`):

| Command | Arguments | Result |
| --- | --- | --- |
| `diag_inspect` | `unit` | position, direction, damage, hit zones, weapons and magazine, fire target, AI brain, move / phase / queue |
| `diag_groups` | `side?` | every AI group: units, alive, leader, spread, still units, formation, semaphore |
| `diag_near` | `unit` or `pos:[x,z]`, `r?` (15) | objects within r: class, shape, type, damage, which LODs exist |
| `diag_geometry` | `unit`, `draw?`, `ttl?` | LOD summary and convex components (geometry, fire, view); boxes in the overlay with `draw` |
| `diag_perf` | — | FrameProfiler window: fps, avg / p95 / max, per-phase stats, counters, paused |
| `diag_pause`, `diag_resume` | — | the dev pause |
| `diag_step` | `ticks?` (1) | run N fixed ticks, one per frame, then stay paused |
| `diag_watch` | `unit`, `on?` | label in the overlay; log its moves, AI changes (`ai`) and trail (`trail`) |
| `diag_mark` | `text` | a `mark` line in events.jsonl |
| `diag_draw` | `on` | overlay on / off at run time |
| `diag_stuck` | `sec` | stuck threshold |
| `diag_anim` | `unit` | primary / secondary move, RTM, keyframes, step, speed, blend |
| `diag_pose` | `unit, move, time?, pause?` | a man in a move at a fixed phase, then pause (pose contact sheets) |
| `diag_kill` | `unit, move?` | `setDammage 1`, optional forced move; the corpse is followed (`corpse` after 6 s) |
| `diag_movecheck` | `cfg?` | CfgMoves* check: RTM files exist, `connectTo` / `interpolateTo` / actions name real moves, death moves terminal, unreachable moves |
| `diag_scripts` | — | running SQS scripts, current line and statement |
| `diag_camera` | `unit, view?, az?, elev?, dist?, height?, follow?` / `off` | pin the render camera on a unit |

Scripts (in the git-excluded `.tw\`, not on the branch): `.tw\test-steps.ps1` plays a JSON step list
(`t` or `after`, `exec`, `down` / `up`, any harness `cmd`, `shot`) against `G:\CWA-OLI\OpenPoseidon.exe`
with the same launch line as `.tw\test-float.ps1`, under `scripts/with-game-lock.sh` (it refuses to run
without `LOCK_OWNER`). `-Diag` adds `--diag <out>\diag` (and `-DiagArgs`). It writes `steps.txt`,
`responses.jsonl`, `probe.txt`, `problems.txt` (BOOT / PROBE / CMD / SCRIPT / GAME checks) and one
`result.txt` (DEPLOYED-FROM stamp, problems, event counts, summary.json, engine.log ERRR / CRIT lines).
`.tw\steps-template.json` is a starting step list; `.tw\tools\evsum.py` summarises an events.jsonl;
`.tw\tools\posesheet.py` / `viewsheet.py` make pose contact sheets.

```sh
LOCK_OWNER="diag smoke" scripts/with-game-lock.sh powershell -NoProfile -File .tw/test-steps.ps1 \
    -Label diag1 -Mission <mission folder> -Steps .tw/steps-template.json -Diag -DiagArgs '--diag-draw'
python .tw/tools/evsum.py .tw/shots-diag1/diag/events.jsonl --unit s0
```

## Build switch

CMake option `POSEIDON_ENABLE_DIAG` (top-level `CMakeLists.txt`), default ON except for the `Release` build
type; it sets `POSEIDON_DIAG=1/0` as a PUBLIC definition on `Poseidon`. OFF removes the module and every hook.
This follows how OP gates the rest of `Dev/`: compiled in, gated at run time (`BuildInfo::ReleaseBuild`
refuses the flags, `--harness` is needed for the commands).

## Verification

* `win-x64-clang-rwdi`, option ON: full build clean. `PoseidonTests.exe "DIAG-001*"`: 13 cases,
  224 assertions, all pass. Full `PoseidonTests.exe`: 3634 cases, 3567 passed, 15 failed, 52 skipped — the
  same 15 failures as the base 95a9eb1d (3621 cases, 15 failed), compared by name.
* Same preset with `-DPOSEIDON_ENABLE_DIAG=OFF` (separate build dir): game, server and tests build clean;
  the 9 option-independent cases pass.
* Evidence: `docs/roadmap/evidence/diag-w1-20261003.json`.

## Not done / not verified

* **No in-game run.** Nothing here has been launched: no events.jsonl from a real mission, no harness command
  answered by a running game, no overlay seen, no `diag_step` / `diag_camera` / crash event observed. Every
  hook compiles and the pure logic is tested; the runtime paths are unverified.
* `.tw\test-steps.ps1` was syntax-checked with the PowerShell parser only, never run.
* The overlay's projection is the Zeus marker projection, assumed rather than seen to line up on wgpu.
* `diag_step` with the dev panel's own pause toggle used at the same time is untested.
* `--diag-fixed-dt` depends on `OpDiag::Init` running before the first `LockstepPacingActive()` call; the code
  path says so, nothing has measured it.
* Linux: `SetCrashNotify` is a no-op there, so no `crash` event.
