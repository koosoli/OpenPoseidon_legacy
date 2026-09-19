#pragma once

#include <atomic>
#include <cstdlib>

namespace Poseidon::InfantryCombat
{
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
