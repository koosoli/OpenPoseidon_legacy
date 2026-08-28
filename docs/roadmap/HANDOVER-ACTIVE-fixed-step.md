# ACTIVE HANDOVER — ARCH-001 fixed-step accumulator

**This file is live. Whoever works on this branch updates it in the same commit as
the code.** It is not a dated historical handover; it is the current state of one
piece of work, written so another agent can take over mid-stream.

| | |
|---|---|
| **Branch** | `codex/arch-001-fixed-step`, tracking `safety/codex/arch-001-fixed-step` |
| **Worktree** | `.claude/worktrees/codex-arch001-fixed-step` (already CMake-configured) |
| **Base** | `consolidated-v2` (trunk since 2026-08-28 — see CLAUDE.md 0c) |
| **Plan of record** | `docs/roadmap/decisions/PHY-001-what-to-port-from-the-fork-20260828.md` and ARCH-001 item **C** |
| **Deployed?** | **No, deliberately.** The shared install is stamped `consolidated-v2`. This branch is not trunk; deploying it would put the owner on unmerged work. |

## Why this work exists

ARCH-001 §1.5 found the engine has **no fixed timestep**: `World::Simulate(float
deltaT, bool&)` takes a variable frame delta. Its §6 schedules the accumulator
**before** any physics library, because otherwise the physics gets written against
a variable delta and has to be redone. PHY-001 §0 records that the warning already
came true once — the ballistics aim solver runs at a fixed 1/500 s while
`ShotShell::Simulate` integrates with the frame delta, so the shot the AI predicts
is reproducible and the shot that flies is not.

---

## State: what is done

All commits are pushed. Nothing is local-only.

| commit | what |
|---|---|
| `985a5e8a` | `FixedStepAccumulator` + unit tests |
| `5580e041` | extracted `World::StepSimulation` — the seam |
| `93b97349` | AI + vehicles on the fixed step |
| `9c6134c0` | AI visibility joins the same tick |
| `a489475c` | world clock, terrain weather, wind join it |
| `5b6c49bb` | simulate **before** presenting, so a frame shows the tick it just ran |
| `0a35211f` | dev-panel **Fixed Step** tab — the accumulator is now observable |

`FixedStepAccumulator` (`engine/Poseidon/Core/Game/FixedStepAccumulator.hpp`):
1/60 s default, max 8 catch-up steps, keeps at most one unsimulated step after the
cap so a slow frame cannot build a permanent backlog, and exposes `Alpha()` for
interpolation.

**Now fixed-step:** world clock (`Glob.time`), `SimulateLandscape` (weather, wind),
`PerformAI`, `SimulateAllVehicles`, sensor/visibility update — all inside
`World::StepSimulation`.

**Still frame-driven, correctly so:** title/cut effects, free camera, camera
effect, HUD, zoom, sound. That is presentation and belongs at the display rate.

## State: what is verified

* Focused unit suite passes — 8 assertions, 2 cases, `[world][time]`.
* `PoseidonGame` builds clean in this worktree.
* **The game runs.** Booted against real game data with `devtest-dawn.abel`,
  rendered, screenshotted: no errors, asserts, panics or exceptions in 874 log
  lines. This was the first time anything on this branch had been run rather than
  unit-tested — do not assume a later slice is fine because this one was.

## What is NOT verified

* No performance measurement under load, and **no play-session reading of the new
  Fixed Step tab**. The tab makes the numbers available; nobody has looked at them
  while actually playing.
* No mission/integration run (Trident). Only one dev mission, one screenshot.
* Multiplayer untouched and unconsidered. `IsSimulationEnabled()` takes a
  different branch under `GModeNetware`; nothing here was reasoned about for it.

---

## Next steps, in the order they should be taken

### 1. Fixed-step diagnostics — DONE (`0a35211f`)

Dev panel → **Fixed Step**. Shows ticks this frame, alpha, ticks-per-frame with
the implied frame rate, worst frame, and catch-up-cap hits, plus a plain verdict
line saying which clock is ahead. `Dev/Diag/FixedStepStats.*` publishes; the tab
renders. Publishing is four relaxed atomic stores on the sim path.

**THE OWNER STILL HAS TO READ IT.** Nobody has seen these numbers in a real play
session — the automated capture proves it boots, not what the ratio is on this
machine under load. That reading is the input to step 2, so ask for it before
starting any interpolation work.

### 2. Camera / entity interpolation — GATED on the step-1 reading

Do not start until the Fixed Step tab has been read during actual play. The tab
prints the verdict directly:

* *"Rendering OUTRUNS simulation"* (ticks/frame < 1) — interpolation pays off.
* *"Simulation outruns rendering"* (> 1) — **do not build it**; every frame
  already shows fresh state and the work would buy nothing.

`Alpha()` exists and nothing uses it. Without interpolation, rendering above the
simulation rate shows the same state twice.

**Scope warning:** this is not a camera-only change. Every rendered entity moves at
60 Hz, so a correct fix needs previous + current transforms per entity, which is a
broad change to the entity system. Do not start it on the assumption that it is
small.

### 3. Scripts — DEFERRED, and the reason matters

`SimulateScripts()` (called at `World.cpp:930`, defined at `WorldSetup.cpp:1423`)
looks like the obvious next move into `StepSimulation`. **It is not, and moving it
naively will break missions.**

It runs today **outside** the `IsSimulationEnabled()` check, deliberately. That
function returns false for a dev pause, an open pause menu, a warning message, lost
window focus, and `_enableSimulation == false` — which includes cutscenes, where
camera scripts must keep running. Moving the call inside `StepSimulation` stops
scripts in every one of those states. No unit test would catch it; only playing
would.

There is a second change that survives even a correct implementation: scripts are
polled once per **frame** today. Fixed-step means 60 polls/second — fewer above
60 fps, and *more* below it via catch-up. OFP scripts get a time budget per call,
so this changes how fast missions progress. Time-based is the more correct
behaviour, but it is a gameplay change that needs mission playtesting, not code
review.

**If you do it:** fixed-step while simulation is enabled, one call per frame while
it is disabled (preserving cutscenes), behind a switch that is OFF by default until
missions have been played.

### 4. Wire the accumulator at the game loop

`GameLoop.cpp:60` still welds the sim and render ticks. The accumulator currently
lives inside `World::Simulate` as a function-local static. That is fine for now but
is not where it belongs long-term.

### 5. Only then: PHY-010

Physics binding per PHY-001. Do not start it before the accumulator is settled —
that is the whole reason ARCH-001 put item C first.

---

## Traps specific to this branch

* **Build from THIS worktree's build directory** (`build/win-x64-clang-rwdi` inside
  the worktree). Tests have gone green in the main checkout against source a
  worktree never changed.
* **Do not deploy from here.** `scripts/Deploy.ps1` guards it, and the owner plays
  the trunk build. If you want to see a change, build `PoseidonGame` in the
  worktree and run its `dist\x64-win-rwdi\OpenPoseidon.exe` through the capture
  harness instead.
* **One game instance at a time.** Check `tasklist` for a running `OpenPoseidon`
  before launching; parallel launches have crashed the agent host.
* `World.hpp` is widely included — changing a signature there triggers a broad
  rebuild. Budget for it.
* Avoid `#if _ENABLE_CHEATS` around anything that changes a method signature; it
  produced a config-dependent signature once already on this branch.

## Rules that apply (CLAUDE.md)

0c — rebase from `consolidated-v2`, never merge. 0d — own worktree, never
`git add -A`, `git diff --stat HEAD` after committing to prove the commit is whole,
push immediately, and if you commit someone else's work say so in the first line.

---

*Last updated: 2026-08-28, after `0a35211f`.*
