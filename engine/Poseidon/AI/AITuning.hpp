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

} // namespace Poseidon::AITuning
