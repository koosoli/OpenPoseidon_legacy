#pragma once
#include <algorithm>
#include <cmath>

namespace Poseidon
{
// Render reach only: objectsZ still governs simulation. Fog-limited terrain
// must cover every admitted object, including retained tree silhouettes.
inline float ResolveObjectDrawRange(float legacy, float fogRange, float scale)
{
    float requested = scale > 0.0f && std::isfinite(scale) ? legacy * scale : std::max(legacy, fogRange);
    requested = std::max(legacy, requested);
    if (std::isfinite(fogRange) && fogRange > 0.0f)
        requested = std::min(requested, fogRange);
    return requested;
}
}
