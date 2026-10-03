#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace Poseidon
{
// Liquid streaks are a camera-local visual population, not precipitation
// parcels that must be tracked all the way from an aircraft to distant terrain.
// Leaving this cylinder is neither a ground hit nor a shelter collision.
inline bool RainParticleInViewVolume(std::array<float, 3> particle, std::array<float, 3> camera,
                                     float radius, float height)
{
    if (!std::isfinite(radius) || !std::isfinite(height) || radius <= 0 || height <= 0)
        return false;
    for (unsigned i = 0; i < 3; ++i)
        if (!std::isfinite(particle[i]) || !std::isfinite(camera[i]))
            return false;
    const double dx = double(particle[0]) - camera[0];
    const double dy = double(particle[1]) - camera[1];
    const double dz = double(particle[2]) - camera[2];
    return dx * dx + dz * dz <= double(radius) * radius && std::abs(dy) <= height;
}

inline int RainParticlePopulationLimit(int target, int maximum, float density)
{
    if (target <= 0 || maximum <= 0 || !std::isfinite(density) || density <= 0)
        return 0;
    const int boundedTarget = std::min(target, maximum);
    return static_cast<int>(std::ceil(double(boundedTarget) * std::clamp(double(density), 0.0, 1.0)));
}
} // namespace Poseidon
