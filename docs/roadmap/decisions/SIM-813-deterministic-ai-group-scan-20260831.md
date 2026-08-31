# SIM-813 — The AI stage stopped asking the CPU clock which groups get to think

**Date:** 2026-08-31
**Branch:** `agent-ai-determinism`, rebased onto `safety/asset-obs-001` at `a6b8a051`
**Roadmap:** Phase 8 — deterministic simulation, prerequisite for command/commit and replay.

**Related:** [SIM-812](SIM-812-deterministic-command-commit-20260831.md) (whose top-ranked
finding this is), [SIM-808](SIM-808-first-divergent-boundary-20260831.md) (`StateTimeline`,
the oracle used here), [SIM-811](SIM-811-serial-task-graph-20260831.md) (the stage graph),
[SIM-803](SIM-803-stable-iteration-and-tie-breaking-20260831.md) (ordering precedent),
[SIM-810](SIM-810-the-simulation-step-as-it-actually-runs-20260831.md) (the step as it runs).

SIM-812 closed with: *"This should be fixed — a fixed group-per-tick budget, or a budget
denominated in simulated work — before any command/commit work on `PerformAI`."* This is that
fix. It is 6 lines of engine change, and most of this document is the audit that says why
those particular 6 lines and not the other candidates.

---

## 1. What was measured

### 1.1 The defect, verified

`AICenter::UpdateGroup` (`engine/Poseidon/AI/AICenterImplPreview.cpp:480-524` before this
change) opened with `Foundation::StartSectionTime()` — `QueryPerformanceCounter`
(`Foundation/Framework/PoTime.cpp:81-84`) — and ended each iteration with

```cpp
if (Foundation::CompareSectionTimeGE(sectionTime, maxTimePerCenter)) break;   // 0.020f
```

so the number of groups that thought in a tick was a function of elapsed CPU time.

**`maxTimePerCenter` is set in exactly one place and to exactly one value.** It is a function-
local `float maxTimePerCenter = 0.020;` — not a config key, not an `ENGINE_CONFIG` field, not
tunable, not serialized, and not read anywhere else. `grep -rn "maxTimePerCenter" engine/`
returns that one line. So there is no denominator question about *who set it*: nobody, ever,
other than that literal. 20 ms per center per tick, on a 16.67 ms fixed step.

**It was the only wall-clock budget in the simulation.** `grep -rn
"StartSectionTime\|CompareSectionTime\|SectionTimeHandle" engine/` excluding
`Foundation/` returns **2 lines, both in this function**. That matches SIM-812's count and I
re-ran it rather than taking it on trust.

### 1.2 The wider sweep — is any other simulation decision clock-derived?

Searched `QueryPerformanceCounter`, `GetTickCount`, `timeGetTime`, `clock()`,
`std::chrono::{steady,system,high_resolution}_clock`, `GetRealTime`, `SystemTime` across
`engine/Poseidon/AI`, `World`, `Game` and `engine/Evaluator`. **Denominator: 28 `chrono` call
sites in the sim/world tree, plus 5 `QueryPerformanceCounter` sites.** Classified all of them:

| Class | Count | Feeds a simulation decision? |
|---|---:|---|
| `QueryPerformanceCounter` inside `Foundation/Framework/PoTime.cpp` | 4 | No — the implementation of the timing API itself |
| `QueryPerformanceCounter` reached from `UpdateGroup` | 1 | **Yes. This document.** |
| `chrono` in `SmokeVolume`, `PhysicsWorld`, `BuildingInterior`, `SceneShadowPass`, `SceneDraw`, `LandscapeRender`, `ObjectStreamPrepare` | 23 | No — every one writes a `_lastXxxUs` / `_stepMs` stat or a log line. Traced each to its consumer; none is read back into a branch |
| `chrono` in `AICenter.cpp:628` | 1 | No — builds the filename of the MP report (`mpreport_<timestamp>.txt`) |
| `chrono` in `LandSave.cpp` object-stream admission (`admitStart` / `elapsedMs`, `:3452`, `:3515`) | 3 | **Yes, but not simulation.** A slow frame admits *more* objects. This decides terrain-object *residency*, i.e. what is drawn and streamed, not what is simulated. Named here so nobody rediscovers it and calls it a sim bug; it belongs to the streaming roadmap, not this one |

So: one simulation site, and it is the one SIM-812 named. **This is a site, not a pattern**,
and that claim now has a denominator behind it rather than a spot check.

### 1.3 The finding that decided the design: the step is already fixed

`World::Simulate` runs the whole simulation on a **fixed 1/60 s step**
(`World.cpp:904-916`, `FixedStepAccumulator`, `DefaultStepSeconds = 1.0/60.0`, with a
`_maxCatchUpSteps` clamp). `Glob.time += step.deltaT` inside the `Clock` stage
(`World.cpp:1969`) therefore advances by a **constant**; only the *number* of steps run per
rendered frame varies with real time.

That matters more than anything else in this audit. It means the simulation is already
"given tick index N, the contents of tick N are a function of state" *except* for the AI
budget. Removing the budget does not leave the AI stage one clock dependence among many — it
leaves it with none.

### 1.4 How many groups get skipped in a real tick — stated honestly

**I cannot measure this, and I did not.** Another session owns the game folder
(`D:\SteamLibrary\...\ARMA Cold War Assault`); per the brief I did not deploy and did not
launch. There is no existing counter for it, so no log from a past session can answer it
either. What can be established from the code:

- **`GroupsPerCycle` is 1** (`AICenterImplPreview.cpp:69`). The loop runs until **one** group
  reports it did work, not until *N* have thought.
- **The walk is bounded by `NGroups()` in the ordinary case.** Each iteration executes
  `maxGrp->_lastUpdateTime = _lastUpdateTime;`, and the selection compares strictly
  (`if (maxAge < age)`) against `maxAge = 0`, so a stamped group has age exactly 0 and can
  never be picked again inside the same call. Null slots are skipped without being stamped.
  So the visited set is at most every non-null group, once.
- **What makes the walk long is `Think()` returning false.** `AIGroup::Think`
  (`AIGroup.cpp:1200-1459`) returns `someOperPath` from its subgroups, and returns `false`
  early for a group with no leader, a logic-side group, a **non-local** group, and a group
  that destroyed itself mid-think. A local group can also run its *entire* think — including
  the once-per-second expensive path (`Glob.time > _expensiveThinkTime`, `CreateTargetList`,
  `AssignTargets`, `AssignVehicles`, `CheckSupport`) — and still return false. So the
  expensive case is not rare-by-construction: on a side whose groups mostly report no path
  work, one center tick can run a full think for every group it owns.
- **The one way past `NGroups()` is group creation inside `Think()`.** `AIGroup::AIGroup`
  sets `_lastUpdateTime = Glob.time - 1.0F` (`AIGroup.cpp:53`) — a newborn group is
  immediately one second old, is therefore the oldest, and is picked next. That is an
  unbounded loop with nothing but the clock stopping it.

**Is the budget load-bearing for framerate? Partly, and the honest answer has two halves.**
It cannot be load-bearing against the *bounded* case, because the bound is one pass over the
group list and a fast machine already performs that pass today. It *is* load-bearing against
the unbounded creation case, and against a heavy tick on a slow machine. §3 says what this
change does about each.

---

## 2. The design, and the three options rejected

**The requirement:** the set and order of groups that think in a tick must be a function of
simulation state only.

### Rejected: a fixed small group-per-tick constant (SIM-812's first suggestion)

*"A fixed group-per-tick budget"* — e.g. stop after 4 loop entries. Deterministic, bounds a
heavy tick, and **changes which groups think in normal play on every machine**, including the
machines where the old budget never tripped. The roadmap requires behavioural tests before AI
ordering changes, and this option changes ordering unconditionally rather than only where it
was already nondeterministic. Rejected on that ground alone; the framerate argument for it is
real but does not survive the ordering cost.

### Rejected: a work-unit budget derived from state (SIM-812's second suggestion)

Charge each iteration a cost the center can read from state before calling `Think()` — chiefly
whether the group is due its once-per-second expensive path (`Glob.time > _expensiveThinkTime`
is readable at the call site) — and stop at a fixed work total. This is the *right long-term*
answer: it keeps the "stop early on a heavy tick" property while being state-derived.

Rejected **for this change** because it requires a calibrated constant — how many work units a
cheap iteration costs relative to an expensive one — and calibrating it needs the game folder,
which I could not use. An uncalibrated cost model would change think rates in normal play by
an amount I could not state. Shipping a number I could not defend is exactly the failure mode
this repo's rules are written against. It is left as the follow-up, and §5 says what has to be
measured first.

### Rejected: keep the wall clock, gate it behind a determinism mode

Two behaviours, one shipped and one tested, with the tested one never run by a player. The
roadmap's whole point is that the *shipped* simulation is replayable; a determinism mode makes
the gate pass while leaving normal play exactly as nondeterministic as before. It also doubles
the behaviour surface for every future AI change.

### Chosen: the walk's own natural ceiling

`GroupScanBudget(NGroups())` allows `NGroups() + 1` loop entries — the `+1` being the entry
that finds nothing left and breaks on its own, which also runs `_lastUpdateTime = Glob.time`
and starts the next cycle. Because §1.4 establishes that a creation-free walk cannot make more
entries than that, **the ceiling cannot bind on any walk a fast machine would have finished**.
The change is therefore:

- a **literal no-op** wherever the 20 ms budget did not trip — same groups, same order, same
  everything;
- a **bound** on the group-creation case that previously had only a clock to stop it;
- and where the 20 ms budget *did* trip, the walk now completes. That is the fix, not a side
  effect: "how many groups thought" can no longer answer "how fast is this CPU". The worst
  case it admits is exactly the walk a fast machine already performs.

`engine/Poseidon/AI/AIGroupScanBudget.hpp` — 30 lines of code, std-only, no engine
dependencies. One implementation detail worth recording: the accessor is `Ceiling()`, not
`Limit()`, because **`Limit` is a function-like macro in this engine**
(`Foundation/Math/Math3DP.hpp:511`, and two siblings) and a member of that name fails to
compile in any translation unit that has seen the math headers. That cost one build.

---

## 3. What this changes and what it does not

| Situation | Before | After |
|---|---|---|
| Walk completes inside 20 ms (a fast machine, a normal tick) | all reachable groups visited | **identical** |
| Walk exceeds 20 ms (slow machine, or a tick where many groups run their expensive think) | cut short at a clock-dependent point | walk completes; more groups think, and the *same* groups on every machine |
| A group is created inside `Think()` | walk continues until the clock stops it | walk stops at `NGroups() + 1` entries; the newborn is picked on a later tick, oldest-first |
| Center with zero groups | loop breaks on `!maxGrp` | unchanged (`Ceiling()` is 1, spent by the same entry) |

**On the roadmap's "stop and report if ordering changes in normal play" rule.** The ordering
changes only on ticks where the old budget tripped — and on those ticks there was no
well-defined old behaviour to preserve, because the answer depended on the CPU. I do not think
that meets the bar for "changes which groups think in what order in normal play", and I have
shipped it. What I *cannot* rule out without the game folder is that the budget trips often
enough on the owner's machine for the completed walk to cost measurable frame time. §5 says
what to measure and what to do if it does.

---

## 4. The tests, and the ablations

`tests/unit/engine/Poseidon/AI/test_ai_group_scan_determinism.cpp` — 4 cases, 32 assertions,
tag `[determinism][ai][sim813]`, in `PoseidonTests`. No game data.

`AICenter::UpdateGroup` cannot be instantiated in a unit test — it needs a `World`, a `Glob`
and live `AIGroup`s. What *can* be lifted out is the loop's stopping decision, which is 100%
of the defect, and `GroupScanBudget` is that decision **as shipped code**: `UpdateGroup` calls
this class, not a copy of it. The walk around it is a model, stated as one in the file header,
mirroring `AICenterImplPreview.cpp:486-522` line for line.

The equivalence case runs the same simulation state twice, differing **only** in a simulated
per-group `Think()` cost (×1 and ×64) — the thing a real machine's speed varies — and compares
with SIM-808's `CompareTimelines`. Selection is recorded as `Discrete` fields, per SIM-808's
rule that no diagnostic may be applied to a selection outcome.

### Ablations

Three, each forced to fail, each restored, harness output recorded.

```
A1  ABLATION: reintroduce the wall clock at the shipped decision point
    (StopDeterministic also returns true when spent >= budget)

    the deterministic group scan selects the same groups under any machine speed
      CHECK( div.kind == DivergenceKind::None )
      with expansion: 1 == 0
      first divergence at tick 0, entry 0: ai.groupScan[0].visitedCount (discrete)
      0x00000004 vs 0x00000001 -- a discrete outcome differs; no tolerance applies
      CHECK( fast.RunHash() == slow.RunHash() )
      0x1087a4206a88ca33 == 0xcb6151b00aab5013     -> FAILED

A2  ABLATION: shrink the ceiling to NGroups()/2 so the budget really binds

    the scan budget cannot bind on a walk that only visits existing groups
      CHECK( r.visited.size() == kGroups )     8 == 16          -> FAILED
      CHECK( centerTime == 1.0f )              0.0f == 1.0f     -> FAILED
    the scan budget bounds a walk that creates groups under it
      CHECK( entries == kGroups + 1 )          8 == 17          -> FAILED

A3  ABLATION: make Exhausted() never true (the bound removed entirely)

    the scan budget bounds a walk that creates groups under it
      CHECK( entries == kGroups + 1 )          10001 == 17      -> FAILED
      CHECK( budget.Visited() == kGroups + 1 ) 10001 == 17      -> FAILED
```

`centerTime == 1.0f` failing under A2 is the informative one: it is set **only** on the
`maxIdx < 0` path, so a walk that ends on the budget instead of on running out of groups never
starts the next cycle. That is the behaviour change a too-small ceiling causes, and the test
names it.

### Two mistakes this test made first, recorded because they are the reason to trust the third

1. **The first ablation of A1 passed, and it should not have.** `Walk` special-cased the two
   policies internally and called `budget.Exhausted()` directly for the deterministic one, so
   ablating the policy function changed nothing — `Walk` never called it. The harness reported
   *"All tests passed (32 assertions in 4 test cases)"* on an ablation designed to break it. A
   test that cannot fail proves nothing, and this one could not. Fixed by routing **both**
   policies through the same call at both loop positions; the note is in the source so it is
   not re-introduced. SIM-812 hit the same class of error twice; this is the third.

2. **The model was periodic and the anti-vacuity check caught it.** `Thinks(j, t) =
   ((j + t) % 5) == 4` gave **8 distinct tick hashes over 96 ticks** — a run that would have
   agreed with a wrong implementation on 88 of them. Replaced with a well-mixed function of
   the same two state variables (no less deterministic), which gives **54 of 96**. The check
   is `DistinctTickHashes() >= kTicks / 4` and it is not decoration.

The equivalence case also asserts that some tick visits more than one group
(`EntryCountInTick(t) > 2`); a walk that always exits after one iteration could not detect a
budget error at all.

---

## 5. What this does not prove, and what is left

- **It measures nothing in the running game.** No profiling, no frequency measurement, no
  deploy. The game was not launched and `tri` was not run — another session owns the game
  folder. **How often the 20 ms budget actually tripped is unknown**, and I did not add a
  counter for it because a counter nobody reads is worse than none (this repo has four
  examples of diagnostics that reported `0` when they meant *n/a*). The right follow-up is a
  named counter wired into the dev panel by whoever next holds the game folder: *count of
  `UpdateGroup` calls whose walk exceeded 20 ms, per center, per session*. One number decides
  whether the work-unit budget of §2 is needed at all.

- **It does not make AI selection agree across clients.** The walk is per-center and the
  ordering key is `_lastUpdateTime`, which is replicated state — but non-local groups take a
  different path through `Think()` and the group *list* order is not established to be the
  same on two clients. SIM-812's stable-entity-ordinal item covers that; this does not.

- **It does not touch the rest of `PerformAI`.** Neighbour writes, the AI locking grid, the
  shared `GRandGen` stream and the radio queue are all as SIM-812 left them. This removes the
  one thing that made a commit ordering pointless, and nothing more.

- **It does not benchmark the heavy tick.** The claim in §3 is a *bound* — the worst case
  admitted equals the walk a fast machine already performs — not a measurement that the bound
  is cheap.

- **`AICenterImpl.cpp` and `AICenter.cpp` also `#define GroupsPerCycle 1`** and were left
  alone; neither contains a copy of this loop (`grep -n "GroupsPerCycle"` shows the macro
  defined in three files and *used* in one).

## Evidence

Build: `win-x64-clang-rwdi`, worktree `.claude/worktrees/agent-ai-det`, Bash tool plus the SDK
`mt.exe` on `PATH`, not `Build.ps1`.

| run | test cases | assertions | failures |
|---|---:|---:|---:|
| `PoseidonTests` baseline (`153723ce`, pre-rebase) | 3089 | 389,159 | 0 |
| `PoseidonTests` after SIM-813 | 3093 | 389,197 | 0 |
| `PoseidonFoundationTests` baseline | 491 | 173,085 | 0 |
| `PoseidonFoundationTests` after SIM-813 | 491 | 168,343 | 0 |

The `PoseidonTests` difference is exactly the 4 new cases and 38 new assertions. 20 cases skip
in both runs (external game data absent on this box). The Foundation assertion count moves
between runs on unchanged code — those suites are randomly seeded — which is why the failure
column, not the assertion column, is the one that matters there.

**Nothing was deployed.** The owner's game folder is untouched and does not have this build,
by instruction: another session was measuring in it. Anyone deploying this should know that
the AI stage now completes walks it used to abandon, and should watch frame time on a
high-group-count mission before assuming it is free.
