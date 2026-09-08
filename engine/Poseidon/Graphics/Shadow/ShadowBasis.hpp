#pragma once

#include <cmath>

namespace Poseidon::shadow
{
inline bool LocalShadowBasisMismatch(const float* basis, float x, float y, float z, int localViews)
{
    if (localViews <= 0)
        return false;
    const float dx = basis[0] - x;
    const float dy = basis[1] - y;
    const float dz = basis[2] - z;
    const float distanceSquared = dx * dx + dy * dy + dz * dz;
    return !std::isfinite(distanceSquared) || distanceSquared > 1.0f;
}
}
