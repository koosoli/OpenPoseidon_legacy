#pragma once

namespace Poseidon
{
// The midpoint sphere must enclose both rounded ends of a swept sphere.
constexpr float SweptSegmentBoundingRadius(float segmentLength, float queryRadius)
{
    return segmentLength * 0.5f + (queryRadius > 0 ? queryRadius : 0);
}
}
