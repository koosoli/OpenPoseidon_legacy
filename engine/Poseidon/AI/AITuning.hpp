#pragma once

// Runtime AI tuning that a person is expected to change and judge by eye.
//
// PERF-017 measured the one setting in here and PERF-016 argued for it, but neither could
// decide it: rationing the near-target scan removes 91% of its cost AND changes what the AI
// perceives, and "is that still good AI" is a judgement, not a measurement. The owner made
// that call after playing `perf_combat` both ways.
//
// So the value ships as a default rather than a constant, and the AI tab can put it back.
// Zero is the 2001 behaviour exactly: scan on every group think.

#include <cstdint>

namespace Poseidon::AITuning
{

/// The 2001 behaviour: no ration at all.
inline constexpr float kTrackNearTargetsUnrationed = 0.0f;

/// What ships. PERF-017: `track` 3.074 -> 0.267 ms/tick, AI scan 3.821 -> 1.149.
inline constexpr float kTrackNearTargetsDefault = 0.5f;

/// Minimum seconds between near-target scans for one unit. 0 disables the ration.
///
/// Read once per unit per group think on the simulation thread and written from the dev
/// panel, hence atomic -- not because the AI is threaded (it is not), but because a UI
/// write must not tear a value the simulation is reading in the same frame.
[[nodiscard]] float TrackNearTargetsPeriod();
void                SetTrackNearTargetsPeriod(float seconds);

/// True when POSEIDON_AI_TRACK_PERIOD set the value at startup. The panel says so, because
/// a slider that silently disagrees with the environment it was launched under is how a
/// measurement gets attributed to the wrong setting.
[[nodiscard]] bool TrackNearTargetsFromEnvironment();

/// PERF-020. The same shape for the idle-watch target selection: `SelectInterestingTarget`,
/// which every idle soldier ran on every think to decide where his head turns, walking the
/// whole group target list each time. PERF-019 measured it at 94-96% of the settled unit
/// think on both Chernarus+ and Everon. Zero is the 2001 behaviour: walk on every think.
inline constexpr float kWatchSelectUnrationed = 0.0f;

/// What ships. PERF-020: the walk runs once per period per unit, phased by slot; between
/// walks the head keeps following the target it last chose.
inline constexpr float kWatchSelectDefault = 0.5f;

/// Minimum seconds between idle-watch target selections for one unit. 0 disables the
/// ration. Read once per idle soldier per unit think, written from the dev panel; atomic
/// for the same reason as the track period.
[[nodiscard]] float WatchSelectPeriod();
void                SetWatchSelectPeriod(float seconds);

/// True when POSEIDON_AI_WATCH_PERIOD set the value at startup.
[[nodiscard]] bool WatchSelectFromEnvironment();

/// The sentinel that means "no bound at all": `AIGroup::CreateTargetList` runs the whole
/// pass in the call it was made from.
///
/// It is negative rather than a large positive number on purpose. A caller that needs the
/// finished list -- `AICenter::InitSensors` builds the world's sensors from it before the
/// first tick, and a script asking what a group can see cannot be told "half of it" -- must
/// be able to say so in a way that no arithmetic on the counter can accidentally satisfy.
inline constexpr int kTargetListBudgetUnlimited = -1;

/// Counted units of target-list work one `AIGroup::Think` may spend, or
/// `kTargetListBudgetUnlimited`.
///
/// A "unit" is one examined item: a unit slot in the visibility scan, an entry in the
/// report walk, an entry in the centre-database walk, an entry in the subjective-cost walk.
/// It is deliberately NOT milliseconds. `AIGroup::CreateTargetList` decides what the group
/// perceives, and SIM-813 removed the last wall clock from that decision -- a millisecond
/// budget here would put it straight back, and make "what does this group know" a function
/// of how fast the machine ran. A counted budget is the same on every machine and in every
/// replay of the same simulation state.
///
/// Defaults to the sentinel, so the shipped behaviour is the 2001 behaviour: every pass
/// completes in the call that started it.
[[nodiscard]] int  TargetListBudget();
void               SetTargetListBudget(int units);
[[nodiscard]] bool TargetListBudgetFromEnvironment();

} // namespace Poseidon::AITuning
