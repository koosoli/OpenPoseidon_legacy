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
| `f21e6cba` | accumulator becomes a `World` member; Dev counters reset on world load |
| `4a3bf51b` | fixed-step scripts as an **opt-in, off by default** |

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

* Fixed Step tab READ during play — see step 2 for the numbers. Still no profiler
  measurement of what the accumulator itself costs.
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

### 2. Camera / entity interpolation — CANCELLED, measured 2026-08-28

**Do not build this.** The owner read the Fixed Step tab across three play
sessions on the target machine:

| session | ticks/frame | implied fps | worst frame | catch-up cap |
|---|---|---|---|---|
| 1 (1379 ticks / 909 frames) | 1.517 | 39.6 | 8 | 3 frames, 0.33 % |
| 2 (2211 / 1916) | 1.154 | 52.0 | 8 | 1 frame, 0.05 % |
| 3 (1184 / 1051) — clean, reset after load | 1.127 | 53.3 | 2 | never |
| 4 (5089 / 4919) — **under heavy AI load** | 1.035 | 58.0 | 8 | 7 frames, 0.14 % |

Every session reads **above 1.0**: the simulation is the FASTER clock and several
ticks run per drawn frame. Interpolation only pays when rendering outruns
simulation, so here it would buy nothing — every frame already shows fresh state.
Frame rate is the limit, not the simulation.

This matches what the renderer work already measured independently: stock worlds
are GPU-bound and grass alone is about a third of the frame. Reopen this only if
the frame rate ever rises well above 60 on the target hardware.

**Side finding, not a blocker.** Sessions 1 and 2 hit the 8-step catch-up cap on a
handful of frames, meaning simulation time was discarded and gameplay briefly ran
slower than wall clock. Session 3 — reset after the loading screen — peaked at 2
ticks and never capped, so those hits are load-screen spikes rather than
steady-state behaviour. Session 4 capped on 7 of 4919 frames while many AI units
were spawned, so under real load some of these are genuine hitches rather than
load spikes. Still small, still not worth acting on before something else makes
frames longer -- but this is the number to watch when PHY-010 adds per-frame work.

**On "should I feel a difference": no, and that is correct.** AI previously ran
once per rendered frame, so ~50 updates/s at 50 fps; it now runs at a fixed 60.
Too small a change to perceive, and AI decisions hang off second-scale timers
rather than individual ticks. The gain is that behaviour no longer varies with
frame rate -- the same mission used to run measurably differently on a 20 fps
machine and a 144 fps one. That invariance is the prerequisite ARCH-001 wanted
before physics, not a felt improvement.

### 3. Scripts — IMPLEMENTED as an opt-in, OFF by default (`4a3bf51b`)

`POSEIDON_FIXEDSTEP_SCRIPTS=1`, or the checkbox in the Fixed Step tab under
*Experimental*. Both drive one storage location.

The cutscene hazard is handled in code, not by hoping: `SimulateScripts()` runs
outside `IsSimulationEnabled()` on purpose, so the frame-driven call is KEPT
whenever the simulation is off, whatever the switch says. Only the running case
moves into the tick. Cutscenes behave identically in both configurations.

Inside the tick the call sits after the clock (a script reading `time` sees this
tick's value) and before the AI (orders a script issues are acted on in the same
tick, not the next).

**Why it stays off:** scripts are polled once per FRAME today and 60 times a
second when enabled — fewer above 60 fps, more below it via catch-up. OFP scripts
get a time budget per call, so this changes how fast missions progress.
Time-based is the more correct behaviour and it is still a gameplay change.

**To finish this item someone must PLAY missions with it on** — campaign
missions with cutscenes and triggers, not a dev mission. Nothing else is
blocking; the code is done. Both configurations boot and render clean (874 and
871 log lines, zero errors), but that proves neither behaves *correctly*.

### 4. Wire the accumulator at the game loop — PARTLY DONE (`f21e6cba`)

The ownership half is done: the accumulator is a `World` member rather than a
function-local static, so it no longer outlives the world that filled it, and the
Dev counters reset in `World`'s constructor.

**What remains is optional, and measurement says it is not urgent.** `GameLoop.cpp`
still calls `GWorld->Simulate(deltaT, enableDraw)` once per frame (line 143) and
the fixed stepping happens inside. Hoisting the accumulator out to the loop would
let rendering run at a rate independent of simulation — but sessions 1-4 all
measured the simulation as the FASTER clock, so there is no render rate to
decouple toward. Do this when it buys something, not for tidiness.

### 5. PHY-010 — BLOCKED on one decision, measured 2026-08-28

The accumulator work is done, so PHY-010 is unblocked in ARCH-001's sense. It is
blocked on something smaller and entirely mechanical instead.

**`box3d` is not reachable from our pinned vcpkg baseline.** Adding `"box3d"` to
`vcpkg.json` fails at configure time:

```
error: the baseline does not contain an entry for port box3d
```

Measured:

| | |
|---|---|
| our `builtin-baseline` `170bd3bf…` | 2026-06-12 |
| `box3d` added to vcpkg | 2026-07-03 |

So the port exists in the local vcpkg checkout but postdates the baseline by
three weeks. The change was reverted; the tree configures and builds clean.

**Three ways out, and the choice is the owner's because two of them reach past
this branch:**

1. **Bump `builtin-baseline`** to a commit containing box3d. The normal vcpkg
   workflow, and the only one that is correct for every clone — but it moves
   *every* dependency (sdl3, imgui, curl, spdlog, …) by three weeks at once. A
   break there would have nothing to do with physics, and it lands on every other
   agent's worktree at the same time. Wants its own commit and its own build
   verification, not a ride-along on a physics change.
2. **Overlay port** — `vcpkg-configuration.json` pointing at the local
   `C:cpkg\portsox3d`. Touches nothing else, but hardcodes a machine-specific
   path, so it works here and for nobody else.
3. **Vendor box3d** under `thirdparty/` via FetchContent, like corrosion is
   already fetched. Self-contained and baseline-independent; adds a submodule-ish
   dependency to maintain by hand.

Option 1 is the right long-term answer and the riskiest to do casually. Nothing
else in PHY-010 can start until one of these is chosen.

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

*Last updated: 2026-08-28. Steps 1-4 done; step 5 blocked on the vcpkg baseline decision.*
