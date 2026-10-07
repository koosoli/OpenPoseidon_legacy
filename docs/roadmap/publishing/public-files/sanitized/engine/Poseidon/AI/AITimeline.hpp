#pragma once

// Roadmap Phase 13 / step 12S. See
// design notes
//
// The behavioural oracle for AI changes. PERF-015 found that 68% of the AI stage is one
// unrationed loop whose throttle already exists and is never read, and PERF-016 argues
// that enabling it comes before any job system. What blocks it is not the code -- it is
// one line -- but the absence of a way to say what enabling it CHANGES. Rationing target
// tracking is a behavioural change, and a framerate measurement cannot see behaviour.
//
// So: record what the AI decides, per tick, into a `StateTimeline`, and diff two runs.
// The comparison then names a tick, a unit and a field rather than reporting that two
// runs differ somewhere.
//
// Two properties this recording has to have, and both are easy to lose:
//
//  * **Stable identity.** A `TargetId` is `LLink<EntityAI>` -- a POINTER. Recording it
//    would record heap addresses, which differ between two runs of the same build, and
//    the oracle would report a divergence in every tick of every comparison. Units and
//    targets are recorded by `NetworkId` (creator, id), which is assigned at creation
//    and is the same in both runs. SIM-803 already found a real AI defect that was a
//    ranking sorted by heap address; this is the same trap from the diagnostic side.
//
//  * **A deterministic sweep.** `CompareTimelines` compares positionally, so the ORDER
//    of the recording is part of the state. The sweep is centres in a fixed order,
//    groups by index, unit slots ascending -- never a hash map, never "whatever the
//    container yields". A group that appears or vanishes then shows up as a genuine
//    RecordShape divergence rather than as noise.
//
// Off unless `POSEIDON_AI_TIMELINE` names an output path. When off, `RecordAITick` is a
// load of one bool and a branch.

#include <string>

namespace Poseidon
{
class World;
}

namespace Poseidon::AIDiag
{

/// True when `POSEIDON_AI_TIMELINE` is set. Read once, on first use.
[[nodiscard]] bool AITimelineEnabled();

/// Record one simulation tick. Call AFTER the AI stage, inside the fixed step -- not
/// per frame: a per-frame recording would depend on framerate and two runs would not
/// even have the same number of entries.
void RecordAITick(World* world);

/// Write the recording to the path in `POSEIDON_AI_TIMELINE`. Safe to call twice and
/// safe to call when disabled. Returns an error string, empty on success.
std::string FlushAITimeline();

/// Ticks recorded so far. Zero after a run that was supposed to record is the failure
/// worth catching: a timeline that recorded nothing compares equal to another one that
/// recorded nothing, so the gate has to check this rather than trust the verdict.
[[nodiscard]] int AITimelineTickCount();

} // namespace Poseidon::AIDiag
