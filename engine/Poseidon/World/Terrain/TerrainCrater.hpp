#pragma once
#include <cmath>

namespace Poseidon
{
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
