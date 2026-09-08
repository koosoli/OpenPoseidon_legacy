#pragma once

#include <algorithm>

namespace Poseidon
{
inline float BodyWaterSupport(float depth, float contactBias, float deltaT)
{
    // Keep the animated contact points slightly submerged; lift sunken bodies
    // gradually rather than teleporting them from the seabed to the surface.
    return std::min(depth - contactBias - 0.12f, 1.5f * std::max(deltaT, 0.0f));
}
}
