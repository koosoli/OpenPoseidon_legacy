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

    std::atomic<int> targetListBudget{kTargetListBudgetUnlimited};
    bool             targetListFromEnvironment = false;

    std::atomic<float> watchSelect{kWatchSelectDefault};
    bool               watchSelectFromEnvironment = false;
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
        // PERF-020, same rules as the track period: malformed is ignored, never parsed to
        // zero, because zero here means "walk the whole list every tick" and that is the
        // expensive setting, not the safe one.
        if (const char* v = std::getenv("POSEIDON_AI_WATCH_PERIOD"); v != nullptr && v[0] != '\0')
        {
            char*       end = nullptr;
            const float parsed = std::strtof(v, &end);
            if (end != v && *end == '\0' && parsed >= 0.0f)
            {
                s.watchSelect.store(parsed, std::memory_order_relaxed);
                s.watchSelectFromEnvironment = true;
            }
        }
        // Same rule for the target-list budget: a malformed value leaves the sentinel in
        // place rather than being parsed to zero. Zero would be a budget that can never buy
        // anything, and the pass would stall forever instead of merely being unbounded.
        if (const char* v = std::getenv("POSEIDON_AI_TARGETLIST_BUDGET"); v != nullptr && v[0] != '\0')
        {
            char*          end = nullptr;
            const long int parsed = std::strtol(v, &end, 10);
            if (end != v && *end == '\0' && parsed >= -1 && parsed != 0 && parsed <= 1000000)
            {
                s.targetListBudget.store(static_cast<int>(parsed), std::memory_order_relaxed);
                s.targetListFromEnvironment = true;
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

float WatchSelectPeriod()
{
    return Get().watchSelect.load(std::memory_order_relaxed);
}

void SetWatchSelectPeriod(float seconds)
{
    Get().watchSelect.store(seconds < 0.0f ? 0.0f : seconds, std::memory_order_relaxed);
}

bool WatchSelectFromEnvironment()
{
    return Get().watchSelectFromEnvironment;
}

int TargetListBudget()
{
    return Get().targetListBudget.load(std::memory_order_relaxed);
}

void SetTargetListBudget(int units)
{
    // Anything that is not a positive count collapses to the sentinel. In particular a
    // zero: a budget of zero examined items cannot advance a pass, so a suspended pass
    // would never finish and the group's perception would freeze at whatever it last saw.
    const int value = (units < 1) ? kTargetListBudgetUnlimited : units;
    Get().targetListBudget.store(value, std::memory_order_relaxed);
}

bool TargetListBudgetFromEnvironment()
{
    return Get().targetListFromEnvironment;
}

} // namespace Poseidon::AITuning
