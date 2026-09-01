#include <Poseidon/Dev/Diag/FixedStepStats.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace Poseidon::Dev
{
namespace
{

// Relaxed throughout. Every one of these is a counter read by a human at panel
// refresh rate; there is nothing to order against and no reader that can act on a
// torn combination. Sequential consistency here would put a barrier on the
// simulation path to make a debug readout marginally less stale.
std::atomic<std::uint32_t> g_steps{0};
std::atomic<float>         g_alpha{0.0f};
std::atomic<float>         g_stepSeconds{0.0f};
std::atomic<bool>          g_running{false};

std::atomic<std::uint64_t> g_frames{0};
std::atomic<std::uint64_t> g_ticks{0};
std::atomic<std::uint64_t> g_cappedFrames{0};
std::atomic<std::uint32_t> g_maxSteps{0};

// Matches FixedStepAccumulator::DefaultMaxCatchUpSteps. Kept as a local rather
// than including the accumulator header, because this file must not drag the
// simulation's headers into the dev panel's translation units. If the
// accumulator's cap changes, this becomes wrong in the harmless direction: the
// panel under-reports capped frames rather than inventing them.
constexpr std::uint32_t AssumedCatchUpCap = 8;

// One storage location for the scripts opt-in, seeded from the environment once.
// A separate "env said X" flag beside a "panel said Y" flag is how a toggle ends
// up disagreeing with what the engine actually does.
// ON by default since 2026-08-29, after the owner played with it and reported no
// difference. `POSEIDON_FIXEDSTEP_SCRIPTS=0` turns it off again, because one
// playtest is evidence and not proof, and the thing it changes -- mission pacing
// -- is the kind that surfaces in an odd mission weeks later.
std::atomic<bool> g_fixedStepScripts{[]
                                     {
                                         const char* v = std::getenv("POSEIDON_FIXEDSTEP_SCRIPTS");
                                         return !v || std::strcmp(v, "0") != 0;
                                     }()};

} // namespace

std::atomic<std::uint64_t> g_totalTicks{0};

void PublishFixedStepFrame(const FixedStepFrame& frame)
{
    g_steps.store(frame.steps, std::memory_order_relaxed);
    g_alpha.store(frame.alpha, std::memory_order_relaxed);
    g_stepSeconds.store(frame.stepSeconds, std::memory_order_relaxed);
    g_running.store(frame.running, std::memory_order_relaxed);
    // Field-by-field into atomics, so a new member of FixedStepFrame is NOT published until
    // it is added here. `totalTicks` read as 0 for a whole measurement round because of that.
    g_totalTicks.store(frame.totalTicks, std::memory_order_relaxed);

    // A paused world is not a measurement. Counting its frames would drag the
    // ticks-per-frame average toward zero and make a healthy session look starved.
    if (!frame.running)
    {
        return;
    }

    g_frames.fetch_add(1, std::memory_order_relaxed);
    g_ticks.fetch_add(frame.steps, std::memory_order_relaxed);

    if (frame.steps >= AssumedCatchUpCap)
    {
        g_cappedFrames.fetch_add(1, std::memory_order_relaxed);
    }

    // Compare-and-max rather than a plain store: two publishers would otherwise
    // race the maximum downward.
    std::uint32_t previousMax = g_maxSteps.load(std::memory_order_relaxed);
    while (frame.steps > previousMax &&
           !g_maxSteps.compare_exchange_weak(previousMax, frame.steps, std::memory_order_relaxed))
    {
    }
}

FixedStepFrame LastFixedStepFrame()
{
    FixedStepFrame frame;
    frame.steps = g_steps.load(std::memory_order_relaxed);
    frame.alpha = g_alpha.load(std::memory_order_relaxed);
    frame.stepSeconds = g_stepSeconds.load(std::memory_order_relaxed);
    frame.running = g_running.load(std::memory_order_relaxed);
    frame.totalTicks = g_totalTicks.load(std::memory_order_relaxed);
    return frame;
}

std::uint64_t FixedStepFrameCount() { return g_frames.load(std::memory_order_relaxed); }
std::uint64_t FixedStepTickCount() { return g_ticks.load(std::memory_order_relaxed); }
std::uint64_t FixedStepCappedFrames() { return g_cappedFrames.load(std::memory_order_relaxed); }
std::uint32_t FixedStepMaxStepsInFrame() { return g_maxSteps.load(std::memory_order_relaxed); }

bool FixedStepScriptsEnabled() { return g_fixedStepScripts.load(std::memory_order_relaxed); }

void SetFixedStepScripts(bool enabled) { g_fixedStepScripts.store(enabled, std::memory_order_relaxed); }

void ResetFixedStepStats()
{
    g_frames.store(0, std::memory_order_relaxed);
    g_ticks.store(0, std::memory_order_relaxed);
    g_cappedFrames.store(0, std::memory_order_relaxed);
    g_maxSteps.store(0, std::memory_order_relaxed);
}

} // namespace Poseidon::Dev
