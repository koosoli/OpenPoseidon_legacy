#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Poseidon
{
// One retriggerable cosmetic pulse per existing weapon source. Absolute integer
// simulation time makes render sampling independent of a soldier's AI tick.
// It neither advances gameplay RNG nor creates accumulating burst energy.
struct MuzzleFlashPulse
{
    static constexpr int LifetimeMs = 110;
    static constexpr int PeakMs = 20;
    static constexpr float CoreMetres = 0.7f;
    static constexpr float EndScale = 12.0f; // actual 8.4m finite reach
    std::int64_t birthMs = 0;
    float strength = 0;

    void Trigger(std::int64_t nowMs, float value = 1.0f)
    {
        birthMs = nowMs;
        strength = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
    }
    float Energy(std::int64_t nowMs) const
    {
        if (strength <= 0 || nowMs < birthMs) return 0;
        // Unsigned subtraction also closes arbitrarily large clock gaps without
        // signed overflow; ordinary Glob.time values are integer milliseconds.
        const auto age = std::uint64_t(nowMs) - std::uint64_t(birthMs);
        if (age >= LifetimeMs) return 0;
        if (age <= PeakMs) return strength;
        const float tail = float(LifetimeMs - age) / float(LifetimeMs - PeakMs);
        return strength * tail * tail;
    }
};
} // namespace Poseidon
