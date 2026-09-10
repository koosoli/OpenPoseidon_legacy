# AI architecture assessment, 2026-09-10

Recommendation: **2. Incrementally augment the existing architecture.**
Assessment only; no BT, HTN or new tactical behaviour implemented here.

## What actually exists

Paths below are relative to engine/Poseidon.

| Layer | Concrete code and responsibility |
|---|---|
| Side coordination | AI/AICenterImpl.cpp: UpdateGuarding assigns groups to known targets/guard points; support dispatch already exists. This is not a unit-only AI. |
| Mission/group | AI/AIGroup.hpp: AIGroup derives AbstractAIMachine<Mission, AIGroupContext>. AIGroup.cpp: Think checks authority, morale and mission/group state before updating subgroups. |
| Task hierarchy | AI/AICore.hpp: AbstractAIMachine stores a stack of tasks with individual FSMs, supports push/enqueue/pop and serializes both tasks and execution state. A new hierarchy would overlap an existing one. |
| Command executor | AI/AISubgroupFSM.cpp: attack, movement, get-in, support and success/failure states; AIGroupCmd.cpp: IssueCommand selects recipients and creates/reuses subgroups. AIUnit.hpp: Command carries context, target, destination and network identity. |
| Soldier decisions | AI/AIUnitImpl.cpp: Think, SetWatch, CreateStrategicPath and CreateOperativePath handle attention, target validation, formation/vehicle planning modes and path progression. |
| Perception | AI/AIGroupImpl.cpp: CreateTargetList is a resumable counted-work scan. World/Detection/Target.cpp: WhatIsVisible feeds group knowledge. These are mutable observations, not omniscient planner inputs. |
| Vehicles | World/Entities/Vehicles/Air/HelicopterAI.cpp: Autopilot and AvoidGround translate desired movement into flight controls. Vehicle controllers remain below tactical decisions; replacing their continuous control with tree traversal is not useful. |
| Scripting | Game/Commands/GameStateExt.cpp registers command/do operations, setBehaviour and disableAI. Group command contexts distinguish mission, UI, auto and escape orders. These semantics are compatibility contracts. |

Core/FSM/Fsm.hpp uses function-table states, timeouts and indexed variables.
Large command FSMs make combinations and cancellation harder to audit, especially
when transport, combat and script interruption interact. This is a maintainability
concern, not evidence that FSM dispatch causes the many-soldier frame collapse.
The inspected paths do not provide an explicit reusable fire-team role/reservation
planner for suppress-and-flank. That is a narrower gap than "replace all AI".

## BT / HTN value and limits

- BTs could clarify a small executor's priority/interrupt structure, but only with
  explicit running, cancellation, authority and save/load contracts. Re-evaluating
  selectors and visibility for every unit every frame can be more expensive than
  the existing state machines. No general soldier/vehicle BT migration recommended.
- A bounded, optional group-level HTN-like planner could decompose assault/defend
  into existing commands, reserve a supporting element and replan after failure.
  Start with one tactic, not assault, clearing, retreat and combined arms at once.
- Preferred experiment is planner -> existing Command/AISubgroup FSM -> existing
  unit/vehicle primitives. Adding an intermediate BT immediately duplicates the
  executor and creates two sources of truth. Consider it only if a measured
  maintenance problem survives the smaller experiment.
- More unpredictable is not automatically better. Use perceived information,
  plausible alternatives and mission constraints, not hidden player knowledge or
  random tactical changes. Modern Arma/milsim behaviour is a reference for test
  scenarios, not evidence that those engines use a particular BT/HTN design.

Specific modern reference: Bohemia documents Reforger's behaviour-tree editor and
its live soldier-tree inspection, plus selector/sequence nodes with explicit
Running/Success/Fail results. The transferable ideas are observable execution and
clear completion contracts, not a presumption that migrating to BTs improves OFP
tactics or CPU time. This does not establish HTN usage in Reforger or the internal
architecture of every Arma release. Sources:
[Behavior Editor](https://community.bohemia.net/wiki/Arma_Reforger%3ABehavior_Editor),
[Nodes](https://community.bohemia.net/wiki/Arma_Reforger%3ABehavior_Editor%3A_Nodes).

## Integration and compatibility gates

Use AIGroup's update boundary to propose a plan from a completed target-list
snapshot. Commit through IssueCommand and existing subgroup ownership rules;
never inject per-unit destinations behind a live mission/UI command. Use existing
command completion/failure to advance or cancel the plan. Begin with AI-led groups
explicitly opted in by the test mission. Player orders always interrupt.

Preserve waypoint scripts/triggers, formations, hold-fire/combat mode, difficulty
and detection rules, transport assignment, movement/weapon controllers and radio
semantics as baseline. Radio timing and binocular interruption are separate
behaviour changes, not reasons to replace the architecture.

Only the owning simulation authority issues plan actions. Workers, if introduced,
receive immutable facts and return candidates; main-thread validation rejects stale
results before deterministic commit. Do not parallelize AIUnit::Think wholesale:
it touches shared targets, world state and global RNG-dependent scheduling.
Use fixed counted work, stable IDs/tie-breaks and a separate serialized planner RNG,
not elapsed wall time or worker completion order, for gameplay decisions.

Version saves and serialize active tasks, role ownership, cursor, timers and RNG.
Old saves stay on the existing executor. Network locality transfer and mixed-version
sessions need explicit handling; a server-only planner is not automatically a
complete multiplayer solution. Test interrupted commands, destroyed leaders,
boarding, save mid-plan and locality changes before broad rollout.

## Large battles: measure before adding workers

PERF-020 already measured idle-watch rationing: on its matched historical Everon
pair, unitThink fell from 0.639 to 0.184 ms/tick. Its oracle also detected a real
behaviour divergence; this is not an exact-vanilla optimization. Existing 0.5 s
watch scheduling must not be proposed as if absent, nor extrapolated to today's
80+ soldier complaint. See decisions/PERF-020-idle-watch-selection-is-rationed-20260902.md.

Run matched 80/160/320-unit idle and combat scenes, near and camera-away, recording
simulation ms, visibility pairs, path requests, animation/skinning, shadow draws,
GPU time, p95/p99 frame time and memory. Fix seed, tick rate, camera, geometry LOD,
render settings and warmup. Compare a real vanilla run only with equivalent
mission/settings; current evidence does not quantify that comparison.
Prefer eliminating repeated queries and bounding indivisible bursts first. Worker
candidates are snapshot-based visibility broadphase/path candidate preparation,
not arbitrary groups in parallel. Batching also avoids per-soldier job overhead.

## Staged A/B experiment

1. Establish the above baseline and retain the existing state timeline oracle.
   Same configuration must reproduce before interpreting A/B differences.
2. Add an opt-in shadow planner that logs proposals without issuing commands.
   Bound node expansions per tick and replan on events or a low-rate interval.
   Measure CPU, allocation and proposal stability; no gameplay effect in this arm.
3. Enable one suppress/advance-or-flank scenario through existing subgroup commands,
   with timeout/failure fallback and immediate player/script cancellation. Compare
   mission completion, stuck units, friendly casualties and frame tails, not merely
   kill count. Test save/load and fixed-seed repeatability at each stage.
4. Expand only after measurable tactical benefit and acceptable CPU cost. Test
   deterministic commit with 1/2/more workers if workers are actually warranted.
   A third BT layer or larger redesign requires evidence the existing executor is
   the limiting factor. Otherwise retain the simpler hybrid with existing FSMs.

This is a migration proposal, not an implemented performance improvement or a
claim that existing OFP campaigns have been revalidated under new tactics.
