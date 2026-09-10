#pragma once

#include <algorithm>
#include <cmath>

namespace Poseidon::shadow
{
// Match the lighting shader's reach without removing the explicit shadow budget.
inline float LocalShadowReach(float start, float lightScale, float userScale, float cap, bool matchLight)
{
    start = std::isfinite(start) ? std::max(start, 0.0f) : 0.0f;
    userScale = std::isfinite(userScale) && userScale > 0.0f ? userScale : 6.0f;
    lightScale = std::isfinite(lightScale) && lightScale > 0.0f ? lightScale : 10.0f;
    cap = std::isfinite(cap) ? std::max(cap, 2.0f) : 180.0f;
    return std::clamp(start * (matchLight ? std::max(userScale, lightScale) : userScale), 2.0f, cap);
}
} // namespace Poseidon::shadow
