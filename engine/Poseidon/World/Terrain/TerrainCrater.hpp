#pragma once
#include <cmath>
#include <algorithm>

namespace Poseidon
{
// Config blast damage is a gameplay proxy, not TNT mass. Anchor to classic LAW
// (indirectHit=150), with bounded cube-root scaling for crater dimensions.
inline float BallisticCraterScale(float indirectHit)
{
    if (!std::isfinite(indirectHit) || indirectHit <= 0) return 0;
    return std::clamp(std::cbrt(indirectHit / 150.0f), 0.25f, 2.0f);
}
inline float TerrainBrushWheelRadius(float requested, float wheelSteps)
{
    if (!std::isfinite(requested)) requested = 8.0f;
    requested = std::clamp(requested, 0.1f, 250.0f);
    if (!std::isfinite(wheelSteps)) return requested;
    return std::clamp(requested * std::pow(1.15f, std::clamp(wheelSteps, -64.0f, 64.0f)), 0.1f, 250.0f);
}

inline float TerrainBrushRadius(float requested, float spacing)
{
    // The furthest point from a grid vertex is a cell centre: sqrt(0.5) cells.
    // A little support beyond that reaches a vertex everywhere without forcing
    // a three-cell-wide bowl when the user wants a small brush.
    return std::max(std::clamp(requested, 0.1f, 250.0f), spacing * 0.75f);
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
