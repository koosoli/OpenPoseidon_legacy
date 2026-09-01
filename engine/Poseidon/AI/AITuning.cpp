#include <Poseidon/Foundation/PoseidonPCH.hpp>

#include <Poseidon/AI/AITuning.hpp>

#include <atomic>
#include <cstdlib>

namespace Poseidon::AITuning
{

namespace
{

struct State
{
    std::atomic<float> trackNearTargets{kTrackNearTargetsDefault};
    bool               fromEnvironment = false;
};

State& Get()
{
    // Built in place, not returned from a lambda: State holds an atomic and is therefore
    // not copyable, so the usual `static State s = []{...}();` does not compile.
    static State s;
    static const bool once = []
    {
        // The environment wins at startup so a measurement run can pin the value without
        // touching the panel; the panel can still move it afterwards, which is the point of
        // having it. A malformed value is ignored rather than parsed to zero -- silently
        // turning the ration OFF because someone typed "half" is the wrong failure.
        if (const char* v = std::getenv("POSEIDON_AI_TRACK_PERIOD"); v != nullptr && v[0] != '\0')
        {
            char*       end = nullptr;
            const float parsed = std::strtof(v, &end);
            if (end != v && *end == '\0' && parsed >= 0.0f)
            {
                s.trackNearTargets.store(parsed, std::memory_order_relaxed);
                s.fromEnvironment = true;
            }
        }
        return true;
    }();
    (void)once;
    return s;
}

} // namespace

float TrackNearTargetsPeriod()
{
    return Get().trackNearTargets.load(std::memory_order_relaxed);
}

void SetTrackNearTargetsPeriod(float seconds)
{
    Get().trackNearTargets.store(seconds < 0.0f ? 0.0f : seconds, std::memory_order_relaxed);
}

bool TrackNearTargetsFromEnvironment()
{
    return Get().fromEnvironment;
}

} // namespace Poseidon::AITuning
