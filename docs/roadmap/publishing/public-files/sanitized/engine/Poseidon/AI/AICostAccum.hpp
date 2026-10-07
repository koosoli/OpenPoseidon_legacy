#pragma once

// PERF-014 -- the subdivision of the AI stage.
// See design notes
//
// PERF-013 subdivided the fixed-step tick into its six stages and found `World::PerformAI`
// costing 21-34 ms/tick on `perf_combat` -- the engine's largest single CPU cost, twice the
// next one -- and could say nothing about what inside it costs that. This is the same
// instrument one level down.
//
// The pattern is deliberately `Sim::SimStageCosts()`'s (World/SimStageGraph.hpp): one
// process-wide accumulator, added to UNCONDITIONALLY so there is no gate to forget to switch
// on, reported once a second WITH its denominators, and reset to start the next window. A
// zero therefore means "ran and cost nothing"; a bucket that never runs shows a zero COUNT
// beside it, which is how "did not run" is said here. A diagnostic that cannot distinguish
// those two is the failure mode this project has hit four times in one night.
//
// WALL TIME, DIAGNOSTIC ONLY. Nothing here reaches a simulation decision: no branch, budget,
// or selection anywhere in the AI reads these fields, and the only reader is the reporting
// block in World.cpp. That matters more here than in most places, because the AI stage USED
// to have exactly such a clock -- `AICenter::UpdateGroup`'s 20 ms QueryPerformanceCounter
// budget, which made the set of groups that think a function of CPU speed. SIM-813 removed
// it and SIM-816's determinism mode will report a divergence if one comes back, so this is a
// check that can fail rather than a promise.
//
// NESTING. Four of the buckets are strict subsets of others, because the call chain is
// four deep and the interesting numbers are the DIFFERENCES:
//
//   groupScan   (AICenter::UpdateGroup, inclusive)
//     |__ groupThink   (AIGroup::Think, inclusive)          groupScan - groupThink
//           |            = the oldest-group selection scan, which is the O(nGroups)
//           |              rescan performed once per loop entry
//           |__ groupExpensive  (the once-per-second block: CreateTargetList,
//           |                    AssignTargets, AssignVehicles, the Check* family)
//           |__ subThink        (AISubgroup::Think, inclusive)
//                 |__ unitThink (AIUnit::Think)              subThink - unitThink
//                                = subgroup FSM, formation and refresh work
//
// Summing every bucket therefore does NOT give the stage total. The unnested buckets --
// radio, exposure, map, guarding, support, groupScan, endMission -- do, to within the
// timer overhead, and that is the sum to compare against PERF-013's `ai=` figure.
//
// PERF-015 adds a fifth level, inside `AIGroup::Think`'s own body. PERF-014 measured that
// body at 2.55 ms/tick -- 79% of the AI stage -- ABOVE the descent into subgroups and units,
// and could not say what in it costs that. Five more buckets carve the body into the
// segments the 2001 code already separates, so they are disjoint and their sum is checkable:
//
//   groupThink   (AIGroup::Think, inclusive -- PERF-014)
//     |__ groupThinkEarly  the exits taken before the body starts (no leader, a TLogic
//     |                    centre, a non-local group). Charged by RETARGETING the body timer
//     |                    at whichever exit the call takes, so early and full-body calls are
//     |                    two populations rather than one mean over both.
//     |__ groupThinkFull   the rest -- the whole body, inclusive. early + full == groupThink.
//             |__ groupFlee     the strength/flee check           (AIGroup.cpp:1233-1263)
//             |__ groupFsm       DoUpdate(), the group state graph (AIGroup.cpp:1266-1280)
//             |__ groupJoin      subgroup joining                  (AIGroup.cpp:1282-1312)
//             |__ groupTrack     the TrackNearTargets loop         (AIGroup.cpp:1315-1342)
//             |__ groupExpensive the 1 Hz block                    (AIGroup.cpp:1346-1423)
//             |__ subThink       per subgroup
//
// Those six ARE disjoint and all lie inside groupThinkFull, so
//
//     groupThinkFull - (flee + fsm + join + track + expensive + subThink)
//
// is the body's residual: the combat-mode autodetect arithmetic (AIGroup.cpp:1425-1451),
// `CalculateImportance()`, the loop scaffolding and the timers' own overhead. A residual
// that is a large share of `groupThinkFull` means the segmentation is wrong and nothing below
// it should be believed -- the same arithmetic check PERF-014 applied one level up.

// PERF-019 adds a sixth level, inside `AIUnit::Think` itself. After the group-scan ration
// and the section-binding fix, `subThink` is 70-80% of what remains of the AI stage and
// prior windows said `unitThink` is nearly all of `subThink`. The unit think has a shape the
// 2001 code already separates and the buckets follow it:
//
//   unitThink   (AIUnit::Think, inclusive -- PERF-014, marked at the AISubgroup.cpp:1596
//     |          call site)
//     |__ unitThinkEarly  the exits before `SetWatch()` starts: a non-local unit, a dead
//     |                   one, no subgroup (AIUnitImpl.cpp:1929-1947). Retargeting, as in
//     |                   PERF-015, so early and full calls are two populations.
//     |__ unitThinkFull   the rest, inclusive. early + full == unitThink to within the
//             |           cost of one extra clock pair per call, because this pair is
//             |           INSIDE the function the PERF-014 pair wraps.
//             |__ unitAttack     `SetWatch()`'s auto-attack consideration -- the
//             |                  CommandSent tests and, when they pass, `AttackThink()`
//             |                  (BestFireResult over every weapon/ammo) and
//             |                  `WhatFireResult()`.
//             |__ unitWatch      `SetWatch()`'s watch-mode switch, including
//             |                  `SelectInterestingTarget`, which walks the WHOLE group
//             |                  target list per idle soldier per think and is not
//             |                  rationed. Denominators `unitWatchSelects`/`unitWatchPairs`.
//             |__ unitExpensive  the per-unit `_expensiveThinkTime` block: CheckResources,
//             |                  the away check, target-assigned validation, NVG.
//             |__ unitGetInOut   the three non-planning exits -- CheckGetOut for a
//             |                  non-pilot, CheckGetInGetOut for an AI-less or Stopped
//             |                  unit, including the standing-still test that gates it.
//             |__ unitStrat      the planned-modes preamble: the 10%-drift replan check,
//             |                  RefreshStrategicPlan and `CreateStrategicPath()` (the
//             |                  strategic A*, incremental via ProcessSearching). Marked at
//             |                  the Think call site, NOT inside the callee, because
//             |                  Ship.cpp:1303 also calls CreateStrategicPath and that call
//             |                  is not under unitThink.
//             |__ unitOperTarget `OperPath()` -- picking the next waypoint off the
//             |                  strategic plan (both call sites).
//             |__ unitOperPlan   `CreateOperativePath()` -- the operative planner (all
//                                three call sites). Only entries with `_state == Init`
//                                plan; `unitOperInits` counts those.
//
// The seven leaf buckets are disjoint and inside unitThinkFull, so
//
//     unitThinkFull - (attack + watch + expensive + getInOut + strat + operTarget + operPlan)
//
// is the residual: the guard tests between blocks, the planning-mode switch scaffolding
// and the timers' own overhead. Same rule as PERF-015: a residual that is a large share of
// `unitThinkFull` means the segmentation missed the cost and nothing below it should be
// believed.
//
#include <array>
#include <cstdint>

namespace Poseidon::AICost
{

/// One named cost inside `World::PerformAI`. Order is the order the tick runs them, except
/// for the nested ones, which follow their parent.
enum class Bucket : std::uint8_t
{
    /// `GetRadio().Simulate` for the world, each center, and each of its groups
    /// (WorldImpl.cpp:484-524). Per-group, so its denominator is `radioGroups`.
    Radio,
    /// `AICenter::Think`'s target-database bookkeeping: the `AccuracyLast` expiry walk over
    /// `_targets`, `RemoveOldExposures()` and `AddNewExposures()`.
    Exposure,
    /// `AICenter::UpdateMap()` -- `_map->ProcessChange()` x TargetsPerCycle plus the dirty
    /// field sweep.
    Map,
    /// `AICenter::UpdateGuarding()`. Runs only when `Glob.time >= _guardingValid`.
    Guarding,
    /// `AICenter::UpdateSupport()`. Runs only when `Glob.time >= _supportValid`.
    Support,
    /// `AICenter::UpdateGroup()` inclusive -- the oldest-first walk and everything under it.
    GroupScan,
    /// `AIGroup::Think()` inclusive. Nested inside GroupScan.
    GroupThink,
    /// The `Glob.time > _expensiveThinkTime` block in `AIGroup::Think` (AIGroup.cpp:1346).
    /// The 2001 authors named it expensive; this says whether it still is. Nested inside
    /// GroupThink.
    GroupExpensive,
    /// `AISubgroup::Think()` inclusive. Nested inside GroupThink.
    SubThink,
    /// `AIUnit::Think()`. Nested inside SubThink. The leaf of the AI call chain, and where
    /// path operation (`OperPath`) is reached from.
    UnitThink,
    /// The end-mission scan over `sensorsMap` at the tail of `PerformAI`
    /// (WorldImpl.cpp:600). Not AI, but it is inside the stage and therefore inside the
    /// number PERF-013 attributed to AI.
    EndMission,

    // --- PERF-015: inside `AIGroup::Think`'s body. All nested inside GroupThink, and
    // disjoint from one another and from GroupExpensive and SubThink.

    /// `AIGroup::Think` calls that returned before the body started: no leader
    /// (AIGroup.cpp:1207), a `TLogic` centre (1213, which still runs `DoUpdate()`), or a
    /// non-local group (1222, which still runs `CreateTargetList()` once a second). These
    /// are a different population from a full body think and PERF-014 averaged them
    /// together; `groupThinkFull` is the other population.
    GroupThinkEarly,
    /// `AIGroup::Think` calls that got past those three exits, inclusive of the whole body.
    /// `groupThinkEarly + groupThinkFull == groupThink`, which is the check that the
    /// retargeting is doing what it claims. Its own denominator is
    /// `groupThinksFull + groupThinksDestroyed`.
    GroupThinkFull,
    /// The flee/unflee strength check, `ActualStrength()` included (AIGroup.cpp:1233-1263).
    /// `ActualStrength` walks `MAX_UNITS_PER_GROUP`.
    GroupFlee,
    /// `DoUpdate()` -- the group's FSM state graph, one `Check` state function per call,
    /// plus the destroyed-mid-think guard (AIGroup.cpp:1266-1280).
    GroupFsm,
    /// Leader-subgroup rejoin plus the wait-state subgroup join loop
    /// (AIGroup.cpp:1282-1312).
    GroupJoin,
    /// The near-enemy tracking loop (AIGroup.cpp:1315-1342): for every alive local unit
    /// with an enemy inside 100 m, `TrackNearTargets(_targetList)`, which walks the whole
    /// group target list per unit and is NOT rationed. Denominators `trackScans` and
    /// `trackCalls`.
    GroupTrack,

    // --- PERF-019: inside `AIUnit::Think`. See the level-six diagram above. All nested
    // inside UnitThink; the seven leaves are disjoint and inside UnitThinkFull.

    /// `AIUnit::Think` calls that returned before `SetWatch()`: non-local
    /// (AIUnitImpl.cpp:1929), dead (1935), no subgroup (1947). Retargeting, like
    /// GroupThinkEarly.
    UnitThinkEarly,
    /// `AIUnit::Think` calls that reached `SetWatch()`, inclusive to the return.
    /// `unitThinkEarly + unitThinkFull == unitThink` less one clock pair per call.
    UnitThinkFull,
    /// The auto-attack consideration at the top of `SetWatch()` (AIUnitImpl.cpp:1219-1254),
    /// condition included. `unitAttackThinks` counts the calls that passed the condition
    /// and ran `AttackThink`/`WhatFireResult`.
    UnitAttack,
    /// The watch-mode switch in `SetWatch()` (AIUnitImpl.cpp:1257-1337), including
    /// `SelectInterestingTarget`'s walk of the group target list.
    UnitWatch,
    /// The per-unit `Glob.time > _expensiveThinkTime` block (AIUnitImpl.cpp:1961-2039).
    UnitExpensive,
    /// The three non-planning exits: `CheckGetOut()` for a non-pilot unit,
    /// `CheckGetInGetOut()` for an AI-less or Stopped one (AIUnitImpl.cpp:2041-2070).
    UnitGetInOut,
    /// The planned-modes preamble at the Think call site (AIUnitImpl.cpp:2131-2160):
    /// drift check, `RefreshStrategicPlan`, `CreateStrategicPath`.
    UnitStrat,
    /// `OperPath()` -- next-waypoint selection off the strategic plan.
    UnitOperTarget,
    /// `CreateOperativePath()` -- the operative planner.
    UnitOperPlan,
};

inline constexpr std::size_t kBucketCount = 26;

[[nodiscard]] const char* BucketName(Bucket bucket);

/// Accumulated cost and the counts to divide it by, since the last `ResetAICosts()`.
///
/// PERF-013 measured the sim shedding ticks -- 32 accumulated per second against a nominal
/// 60 -- so ms/tick and ms/second differ by roughly two here and neither is readable without
/// saying which. Every count below exists so a bucket's mean has a stated denominator: 20 ms
/// across four groups and 20 ms across four hundred are different findings and the same
/// millisecond total.
struct CostAccum
{
    std::array<double, kBucketCount> ms{};

    /// `PerformAI` entries with `deltaT > 0` -- the stage's own tick count. It should track
    /// `Sim::SimStageCosts().ticks`; a gap means the AI stage was skipped (`disableAI`).
    std::uint64_t ticks = 0;
    /// `AICenter::Think()` calls. Five centers exist at most (east, west, guerrila,
    /// civilian, logic) and only the non-null ones are counted.
    std::uint64_t centers = 0;
    /// Groups whose radio was simulated -- the per-group loops in `PerformAI`.
    std::uint64_t radioGroups = 0;
    /// `AICenter::UpdateGroup` loop entries across all centers. Each one performs a full
    /// O(NGroups) rescan, so this times `groupsPerScan` is the real work of the selection.
    std::uint64_t scanIters = 0;
    /// Sum of `NGroups()` observed at each `UpdateGroup` entry. `scanIters` x the mean of
    /// this is the comparison count the selection actually performs.
    std::uint64_t groupsSeen = 0;
    /// `AIGroup::Think()` calls. NOT one per center: the walk keeps calling `Think()` until
    /// one reports it did work, so this can reach `NGroups()+1` per center per tick.
    std::uint64_t groupThinks = 0;
    /// Entries into the once-per-second expensive block.
    std::uint64_t groupExpensives = 0;
    /// `AISubgroup::Think()` calls.
    std::uint64_t subThinks = 0;
    /// `AIUnit::Think()` calls.
    std::uint64_t unitThinks = 0;

    // --- PERF-015. The five exits of `AIGroup::Think`, counted separately so the mean of
    // `groupThink` is not a mean over two populations. These five sum to `groupThinks`.

    /// Returned at AIGroup.cpp:1207 -- no leader. Costs a pointer test.
    std::uint64_t groupThinksNoLeader = 0;
    /// Returned at AIGroup.cpp:1219 -- a `TLogic` centre. Costs one `DoUpdate()` if local.
    std::uint64_t groupThinksLogic = 0;
    /// Returned at AIGroup.cpp:1230 -- a non-local group. Costs a `CreateTargetList()` at
    /// most once a second, nothing otherwise.
    std::uint64_t groupThinksRemote = 0;
    /// Returned at AIGroup.cpp:1272/1279 -- the group was destroyed inside `DoUpdate()`.
    /// Rare; a nonzero count here means the flee check and the FSM ran and the rest did not.
    std::uint64_t groupThinksDestroyed = 0;
    /// Reached the end of the body. `groupThinksFull + the four above == groupThinks`; if
    /// they do not match, a return path was added and not counted.
    std::uint64_t groupThinksFull = 0;
    /// Of the full-body calls, those that returned `true` (some subgroup is operating a
    /// path). Only these advance `AICenter::UpdateGroup`'s loop counter, so
    /// `groupThinks - groupThinksTrue` is what the walk pays to find one.
    std::uint64_t groupThinksTrue = 0;

    /// Full-body thinks that entered the near-enemy tracking block -- i.e. had an enemy
    /// inside 100 m. `groupTrack` divided by this is what the block costs a combat group.
    std::uint64_t trackScans = 0;
    /// `TrackNearTargets` calls inside that block. Its own denominator: `trackCalls /
    /// trackScans` is the mean number of engaged units per group.
    std::uint64_t trackCalls = 0;
    /// Sum of `_targetList.Size()` over those calls -- the number of (unit, target) pairs
    /// the block actually visits, which is the work `groupTrack` measures. `TargetList` has
    /// no size cap: `TargetList::Manage` (Target.cpp:387) keeps every entry within
    /// TACTICAL_VISIBILITY of any group member, so the list length is a scene property and
    /// `trackCalls` alone cannot say how big the walk was. `groupTrack / trackPairs` is the
    /// per-pair cost and is the number that says whether this scales.
    std::uint64_t trackPairs = 0;

    // --- PERF-019. Denominators for the unit-think buckets. The three populations of
    // `AIUnit::Think` sum: `unitThinksEarly + unitThinksFull == unitThinks`; if they do
    // not, an exit was added before `SetWatch()` and not counted.

    /// Returned before `SetWatch()` -- non-local, dead, or subgroup-less.
    std::uint64_t unitThinksEarly = 0;
    /// Reached `SetWatch()`.
    std::uint64_t unitThinksFull = 0;
    /// Of the full thinks, those that reached the planning switch -- i.e. the unit pilots
    /// its vehicle, has AI and is not Stopped. `unitThinksFull - unitThinksPilot` is the
    /// population whose think ends in `unitGetInOut` or the residual guard tests.
    std::uint64_t unitThinksPilot = 0;
    /// Entries into the auto-attack body -- the calls that ran `AttackThink()`. The
    /// `unitAttack` bucket's time divided by THIS is the cost of an attack evaluation;
    /// divided by `unitThinksFull` it is what every full think pays for the condition.
    std::uint64_t unitAttackThinks = 0;
    /// `SelectInterestingTarget` calls -- idle non-player soldiers with auto-target on
    /// whose walk actually ran. PERF-020 rations it; `unitWatchDeferred` is the rest.
    std::uint64_t unitWatchSelects = 0;
    /// PERF-020. Idle-watch thinks that reused the last selection instead of walking.
    /// `unitWatchSelects + unitWatchDeferred` is what `unitWatchSelects` alone was before
    /// the ration, and a zero here with the ration on means the gate is not being reached.
    std::uint64_t unitWatchDeferred = 0;
    /// Sum of the group target-list size over those calls: the (unit, target) pairs the
    /// idle-watch walk visits. The same scene-property caveat as `trackPairs`.
    std::uint64_t unitWatchPairs = 0;
    /// Entries into the per-unit expensive block.
    std::uint64_t unitExpensives = 0;
    /// Entries into any of the three unitGetInOut exits.
    std::uint64_t unitGetInOuts = 0;
    /// Entries into the planned-modes preamble (the unitStrat bucket scope).
    std::uint64_t unitStrats = 0;
    /// Of those, entries where `CreateStrategicPath`'s own guards pass -- `_noPath` or
    /// `_updatePath`, and the 10 s `_waitWithPlan` backoff expired -- i.e. the strategic
    /// A* actually starts or continues a search. Counted just before the call, on the
    /// same condition the callee tests.
    std::uint64_t unitStratSearches = 0;
    /// `OperPath()` calls.
    std::uint64_t unitOperTargets = 0;
    /// `CreateOperativePath()` calls.
    std::uint64_t unitOperPlans = 0;
    /// Of those, calls entering with `_state == Init` -- the ones that build a path
    /// (`CreatePath`/`CopyPath`) rather than verify or early-out.
    std::uint64_t unitOperInits = 0;
};

/// The process-wide accumulator. Read it, print it, `ResetAICosts()` to start a window.
CostAccum& AICosts();
void ResetAICosts();

/// Adds this scope's wall time to `bucket` on destruction.
///
/// A plain RAII pair rather than a macro so the early `return`s inside `PerformAI` (there
/// are two, on mission end) cannot leak a half-taken timer.
class ScopedCost
{
  public:
    explicit ScopedCost(Bucket bucket);
    ~ScopedCost();

    /// PERF-015. Change which bucket this scope's time will be charged to. The clock is not
    /// restarted, so the whole scope still lands in exactly one bucket -- only which one is
    /// decided later.
    ///
    /// `AIGroup::Think` has five exits and does not know which population a call belongs to
    /// until it has taken one. The alternative is a `ScopedCost` duplicated at every
    /// `return`, which is the shape that leaks a bucket the next time someone adds a sixth
    /// exit. Retargeting keeps one timer and one exit accounting.
    void Retarget(Bucket bucket) { _bucket = bucket; }

    ScopedCost(const ScopedCost&) = delete;
    ScopedCost& operator=(const ScopedCost&) = delete;

  private:
    Bucket _bucket;
    std::uint64_t _startNs;
};

} // namespace Poseidon::AICost
