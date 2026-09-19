#pragma once

#include <atomic>
#include <cstdlib>

namespace Poseidon::InfantryCombat
{
// A sustained, observed fire-lane rejection can release a firing pause. It
// grants movement only; every later shot still goes through the safety gate.
struct FireLaneWait
{
    const void* target = nullptr; // identity only, never dereferenced
    float first = 0, last = -100;
    void Clear() { *this = {}; }
    void Blocked(float now, const void* identity)
    {
        if (identity != target || now < last || now - last > 0.75f) first = now;
        target = identity;
        last = now;
    }
    bool Recent(float now, const void* identity) const
    {
        return identity && identity == target && now >= last && now - last <= 0.75f;
    }
    bool ShouldYield(float now, const void* identity) const
    {
        return Recent(now, identity) && now - first >= 0.75f;
    }
};

// Session-local A/B, deliberately not serialized into missions or network state.
inline std::atomic_bool& ImprovedSetting()
{
    static std::atomic_bool enabled{[] {
        const char* legacy = std::getenv("POSEIDON_AI_COMBAT_LEGACY");
        return !(legacy && legacy[0] == '1');
    }()};
    return enabled;
}
inline bool Improved() { return ImprovedSetting().load(std::memory_order_relaxed); }
inline void SetImproved(bool enabled) { ImprovedSetting().store(enabled, std::memory_order_relaxed); }
}
