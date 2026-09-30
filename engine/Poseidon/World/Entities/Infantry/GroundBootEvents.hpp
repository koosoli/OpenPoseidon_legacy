#pragma once
#include <cmath>

namespace Poseidon
{
// Authored sole events are a physical producer, independent of whether sound or
// cosmetic marks are enabled. A silent static/reload pose is not locomotion.
inline unsigned GroundBootEvents(float oldPhase, float advance, float leftEdge, float rightEdge,
                                 bool soundEnabled, float strideX, float strideZ)
{
    if (!std::isfinite(oldPhase) || !std::isfinite(advance) || oldPhase < 0 || oldPhase > 1 || advance <= 0 ||
        !std::isfinite(strideX) || !std::isfinite(strideZ) ||
        (!soundEnabled && std::hypot(strideX, strideZ) <= 0.01f)) return 0;
    unsigned result = 0;
    const auto crossed = [oldPhase, advance](float edge)
    {
        if (!std::isfinite(edge) || edge < 0 || edge > 1) return false;
        // phase+advance is deliberately unwrapped. Include the next cycle's
        // authored event when a real simulation tick crosses the loop seam.
        const float first = edge > oldPhase ? edge : edge + 1.0f;
        return advance >= first - oldPhase;
    };
    if (crossed(leftEdge)) result |= 1u;
    if (crossed(rightEdge)) result |= 2u;
    return result;
}

constexpr bool DominantGroundBootMove(float primaryFactor, bool secondary)
{
    return secondary ? primaryFactor < 0.5f : primaryFactor >= 0.5f;
}
} // namespace Poseidon
