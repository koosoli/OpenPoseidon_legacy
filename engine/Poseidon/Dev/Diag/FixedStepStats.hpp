#pragma once

#include <atomic>
#include <cstdint>

// What the fixed-step accumulator actually did, published once per rendered frame
// so the dev panel can show it.
//
// This exists because the accumulator is invisible from the outside. A frame that
// ran two simulation ticks and a frame that ran none look identical on screen, so
// "is the fixed step working" was previously answerable only by reading the code.
// The one question it is built to settle is whether ENTITY INTERPOLATION IS
// NEEDED AT ALL: interpolation only buys something when rendering outruns the
// simulation, and on a GPU-bound machine it frequently does not.
//
// Deliberately not a ring buffer or a trace. Publishing is on the simulation path,
// so it is four relaxed atomic stores and nothing else; everything derived is
// computed in the panel.

namespace Poseidon::Dev
{

struct FixedStepFrame
{
    /// Simulation ticks executed during the last rendered frame. 0 means the
    /// frame was faster than one step and carried its time forward.
    std::uint32_t steps = 0;
    /// Leftover fraction of a step, 0..1. This is the interpolation alpha a
    /// renderer would use; while it is mostly 0 there is nothing to interpolate.
    float alpha = 0.0f;
    /// Fixed step length in seconds (1/60 unless overridden).
    float stepSeconds = 0.0f;
    /// True while the world is stepping at all -- a paused world publishes false
    /// and resets, and its zero-step frames are not a stall.
    bool running = false;
    /// Cumulative ticks since the process started. The per-frame `steps` above cannot align
    /// two runs -- and counting log lines cannot either once the frame hash is written by the
    /// RENDER THREAD, because the interleaving changes. This is the alignment key: it
    /// identifies the simulation state a published frame belongs to, whoever writes the line.
    std::uint64_t totalTicks = 0;
};

/// Called once per rendered frame from World::Simulate. Cheap by construction.
void PublishFixedStepFrame(const FixedStepFrame& frame);

/// Snapshot for the panel. Also returns the running totals below.
FixedStepFrame LastFixedStepFrame();

/// Frames observed since the last reset, and the simulation ticks within them.
/// Their ratio is the honest answer to "how many ticks per frame am I getting",
/// which a single frame's `steps` cannot give because it is always an integer.
std::uint64_t FixedStepFrameCount();
std::uint64_t FixedStepTickCount();

/// Frames that hit the accumulator's catch-up cap. A non-zero value means the
/// machine could not keep up and simulation time was discarded -- the one number
/// here that indicates a real problem rather than a measurement.
std::uint64_t FixedStepCappedFrames();

/// Largest tick count seen in a single frame since the last reset.
std::uint32_t FixedStepMaxStepsInFrame();

void ResetFixedStepStats();

// ---------------------------------------------------------------------------
// Runs SimulateScripts() inside the fixed tick instead of once per rendered
// frame. ON since 2026-08-29, after the owner played campaign content with it.
// It changes gameplay in two ways at once, both recorded below, and the switch
// stays so it can be turned off if either surfaces later.
//
// 1. SCRIPTS WOULD STOP WHILE THE SIMULATION IS DISABLED. Today SimulateScripts()
//    runs outside the IsSimulationEnabled() check on purpose, and that check is
//    false during a dev pause, an open pause menu, a warning message, lost window
//    focus, and _enableSimulation == false -- which includes cutscenes, where
//    camera scripts must keep running. The call site therefore keeps the
//    once-per-frame path whenever the simulation is off, even with this enabled;
//    only the running case moves into the tick.
// 2. POLLING RATE CHANGES. Scripts are polled once per FRAME today. Fixed-step
//    means 60 polls a second: fewer above 60 fps, and more below it via catch-up.
//    OFP scripts get a time budget per call, so this changes how fast missions
//    progress. Time-based is the more correct behaviour, but it is a gameplay
//    change that needs mission playtesting, not code review.
//
// Seeded from POSEIDON_FIXEDSTEP_SCRIPTS; also togglable in the Fixed Step tab.
bool FixedStepScriptsEnabled();
void SetFixedStepScripts(bool enabled);

} // namespace Poseidon::Dev
