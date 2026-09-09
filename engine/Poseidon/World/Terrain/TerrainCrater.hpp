#pragma once
#include <cmath>
#include <algorithm>

namespace Poseidon
{
inline float TerrainBrushWheelRadius(float requested, float wheelSteps)
{
    if (!std::isfinite(requested)) requested = 64.0f;
    requested = std::clamp(requested, 0.5f, 250.0f);
    if (!std::isfinite(wheelSteps)) return requested;
    return std::clamp(requested * std::pow(1.15f, std::clamp(wheelSteps, -64.0f, 64.0f)), 0.5f, 250.0f);
}

inline float TerrainBrushRadius(float requested, float spacing)
{
    return std::max(std::min(requested, 250.0f), spacing * 1.5f);
}
inline bool RocketCraterGroundContact(float impactY, float groundY, float seaY)
{
    return std::isfinite(impactY) && std::isfinite(groundY) && std::isfinite(seaY) &&
           groundY > seaY && std::abs(impactY-groundY) <= 2.0f;
}
// Compact smooth bowl: zero height and slope at the rim, no change outside.
inline float TerrainCraterDepth(float dx, float dz, float radius, float depth)
{
    if (!(radius > 0) || !(depth > 0) || !std::isfinite(radius) || !std::isfinite(depth)) return 0;
    const float q = (dx * dx + dz * dz) / (radius * radius);
    if (!(q < 1.0f)) return 0;
    const float t = 1.0f - q;
    return depth * t * t;
}
}
