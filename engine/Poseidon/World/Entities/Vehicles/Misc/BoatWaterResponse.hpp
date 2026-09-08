#pragma once

#include <algorithm>

namespace Poseidon
{
// Legacy planing drag is quadratic in draft. A passing swell must not be
// treated as metres of additional wetted hull; retain a bounded wave penalty.
inline float PlaningWaterDragDepth(float under, float underFlat)
{
    return std::clamp(under, 0.0f, std::max(underFlat, 0.0f) + 0.25f);
}
}
