#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Poseidon
{
struct MudWheelProfile
{
    float depthScale = 0.5f;
    float maxDepth = 0.12f;
    int wheels = 4;
    bool enabled = false;
};

inline MudWheelProfile SelectMudWheelProfile(const char* name, bool truckOptIn, bool singlePlayer)
{
    if (!name) return {};
    if (std::strcmp(name, "Jeep") == 0) return {0.5f, 0.12f, 4, true};
    if (truckOptIn && singlePlayer && std::strcmp(name, "Truck5t") == 0)
        return {0.65f, 0.16f, 6, true};
    return {};
}

// Signed longitudinal deceleration, no force at rest and no direction reversal
// from this contribution alone. This is rolling resistance, not a tyre solver.
inline float MudRollingDeceleration(float speed, float layerDepth, float wetness, float seconds,
                                    int admittedWheels, int totalWheels)
{
    if (!std::isfinite(speed) || !std::isfinite(layerDepth) || !std::isfinite(wetness) ||
        !std::isfinite(seconds) || seconds <= 0 || admittedWheels <= 0 || totalWheels <= 0) return 0;
    const float soft = std::clamp((wetness - 0.15f) / 0.85f, 0.0f, 1.0f);
    const float depth = std::clamp(layerDepth / 0.24f, 0.0f, 1.0f);
    const float support = std::clamp(float(admittedWheels) / totalWheels, 0.0f, 1.0f);
    const float magnitude = std::min(1.2f * soft * depth * support, std::abs(speed) / seconds);
    return std::copysign(magnitude, speed);
}
} // namespace Poseidon
