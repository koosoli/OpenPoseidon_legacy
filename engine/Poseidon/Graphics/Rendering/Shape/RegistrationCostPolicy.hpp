#pragma once
#include <cstddef>

namespace Poseidon::render
{
// Scheduling observation only: neither rendering coverage nor a time guarantee.
enum class RegistrationCost { Reusable, NeedsRegistration, Unknown };

template<class BindingReusable>
RegistrationCost ClassifyRegistrationCost(bool present, bool rejected, bool partial,
    bool parked, bool metadataComplete, std::size_t bindings, BindingReusable reusable)
{
    if (!present) return RegistrationCost::NeedsRegistration;
    if (rejected) return RegistrationCost::Reusable;
    // A partial parent's admission may register furniture not represented by its textures.
    if (partial) return RegistrationCost::Unknown;
    if (!parked) return RegistrationCost::Reusable;
    if (!metadataComplete || bindings > 128) return RegistrationCost::Unknown;
    for (std::size_t i = 0; i < bindings; ++i)
        if (!reusable(i)) return RegistrationCost::NeedsRegistration;
    return RegistrationCost::Reusable;
}

inline bool ChargeRegistrationCost(bool enabled, bool cpuCold, RegistrationCost cost)
{
    return cpuCold || (enabled && cost != RegistrationCost::Reusable);
}

// Matches EmitGpuProxies' first normal LOD containing direct proxies. ChildCost
// observes registration ONLY, never recursively traverses the child's proxies.
template<class NormalLevel, class ProxyCount, class ChildCost>
RegistrationCost ClassifyDirectProxyRegistrationCost(int levels, NormalLevel normal,
    ProxyCount count, ChildCost child)
{
    if (levels < 0) return RegistrationCost::Unknown;
    for (int level = 0; level < levels && level < 64; ++level)
    {
        if (!normal(level)) continue;
        const int proxies = count(level);
        if (proxies < 0 || proxies > 128) return RegistrationCost::Unknown;
        if (!proxies) continue;
        for (int i = 0; i < proxies; ++i)
        {
            const auto cost = child(level, i);
            if (cost != RegistrationCost::Reusable) return cost;
        }
        return RegistrationCost::Reusable;
    }
    return levels > 64 ? RegistrationCost::Unknown : RegistrationCost::Reusable;
}
}
