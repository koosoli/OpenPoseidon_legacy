# SIM-810 — The simulation step as it actually runs, against the graph Phase 8 draws

**Date:** 2026-08-31
**Branch:** `asset-obs-001`
**Roadmap:** Phase 8, critical-path step 10S — *"First run the same task graph serially"*

Phase 8 asks for a serial task graph and then for serial equivalence to be proven before
anything runs concurrently. Before either, somebody has to write down **what the order is
today**, because Phase 8's diagram is a proposal and the engine is not obliged to match it.

This is that. No code changed.

## The order, from `World::StepSimulation` (`World.cpp:1905-1990`)

| # | Stage | Call | Note |
|---|---|---|---|
| 1 | debug input | cheat scancodes | `MemoryFootprint` etc.; not simulation |
| 2 | **world clock, weather, wind** | `SimulateLandscape(deltaT)` | "Keep the world clock, weather and wind in the same tick as the vehicles that consume them" |
| 3 | **scripts** | `SimulateScripts()` | **opt-in** (`POSEIDON_FIXEDSTEP_SCRIPTS`), default off |
| 4 | **AI** | `PerformAI(deltaT, noAccDeltaT)` | |
| 5 | **vehicles** | `SimulateAllVehicles(...)` | |
| 6 | **visibility / sensors** | `GetSensorList()->SmartUpdateAll()` | gated by `updateVisibility`, a **cheat toggle** (`Ctrl-Y`), true in normal play |
| 7 | **physics** | one `Step` per tick | player proxy moved *before* the step, deliberately |
| 8 | network, chat, voice | | |

`RenderSnapshot` is published by the **caller**, `World::Simulate`, not by this function.

## Where it differs from Phase 8's diagram, and who is right

Three differences. Two of them are the engine deliberately disagreeing with the plan, and
its reasons are written at the call sites — which is why this document exists rather than a
patch making the code match the picture.

**1. Scripts run BEFORE the AI, not after physics.** Phase 8 puts *"scripts with defined
observable ordering"* second from last. The code puts them straight after the clock, and
says why: *"Placed after the clock so a script observing `time` sees this tick's value, and
before the AI so orders a script issues are acted on within the same tick rather than the
next one."* That is a latency argument and a good one. Moving scripts after physics to match
the diagram would add a tick of delay to every scripted order — a gameplay change, not a
refactor. **The code's position should win and the diagram should be amended.**

**2. Visibility is updated AFTER the AI that consumes it.** Phase 8 puts *"perception /
visibility / navigation queries"* before *"AI decisions"*. The code runs
`SmartUpdateAll()` at stage 6, after `PerformAI` at stage 4 — so **AI decides on the
previous tick's visibility**. This is the one worth investigating rather than blessing:
unlike the script placement it carries no comment, so it is unclear whether it is a
deliberate one-tick lag (cheap, and standard in shipping AI) or an accident of the order
these calls were added in 2001. It is also exactly the kind of dependency a task graph is
supposed to make explicit — under Phase 8 it becomes a declared edge, and then somebody has
to decide which way it points.

**3. There is no "deterministic command and result commit" stage at all.** Phase 8 wants
jobs to *"produce results or mutation commands"* and not modify shared world state directly.
Today every stage mutates in place. That is fine while everything is serial and it is the
single largest piece of work between here and the parallel candidates listed in Phase 8 —
larger than the task graph itself.

Minor: input and network are handled at stage 8, after physics, rather than as Phase 8's
leading *"Input / network apply"*.

## What this makes possible next

The serial graph Phase 8 asks for is close to free: stages 2-7 already exist as six named
calls in a fixed order. Making the order **data instead of a call sequence** is a small
change, and its value is that a graph can then be asserted — that a stage's declared inputs
are the ones it reads.

The equivalence proof Phase 8 requires also has an oracle now. `Core/StateTimeline`
(SIM-808, landed today) records a run as labelled fields per tick and reports the **first**
divergence as a tick, a record, a field and two bit patterns. Running the implicit order and
the explicit graph through it is exactly the "serial equivalence" test the phase gate wants,
and it can fail rather than merely pass.

## What this document does not do

- **It changes nothing.** Two of the three differences above are the engine being right; the
  third is a question, not a defect.
- **It does not settle the visibility ordering.** That needs someone to establish whether
  the one-tick lag is intended, and behavioural tests before it is touched — the roadmap is
  explicit that every AI-ordering change requires them.
- **It covers `StepSimulation` only.** What happens inside `PerformAI` and
  `SimulateAllVehicles` is its own ordering problem, and SIM-803 has already found real
  defects one level down (a ranking sorted by heap address, a comparator containing a cycle).
