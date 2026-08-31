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
};

inline constexpr std::size_t kBucketCount = 11;

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

    ScopedCost(const ScopedCost&) = delete;
    ScopedCost& operator=(const ScopedCost&) = delete;

  private:
    Bucket _bucket;
    std::uint64_t _startNs;
};

} // namespace Poseidon::AICost
