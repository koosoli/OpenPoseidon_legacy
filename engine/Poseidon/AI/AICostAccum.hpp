#pragma once

// PERF-014 -- the subdivision of the AI stage.
// See docs/roadmap/decisions/PERF-014-inside-the-ai-stage-20260831.md.
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
};

inline constexpr std::size_t kBucketCount = 17;

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
