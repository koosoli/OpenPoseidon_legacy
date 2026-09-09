#pragma once

namespace Poseidon
{
// A low-alpha road decal can have the same histogram as glazing. Geometry
// semantics take precedence over the texture-only heuristic.
constexpr bool LegacyGlassAlphaIsGlazing(bool onSurface, double clearPercent, double partialPercent, double meanAlpha)
{
    return !onSurface && clearPercent < 5.0 && partialPercent > 80.0 && meanAlpha < 128.0;
}
}
