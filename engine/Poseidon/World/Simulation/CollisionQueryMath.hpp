#pragma once

namespace Poseidon
{
// The midpoint sphere must enclose both rounded ends of a swept sphere.
constexpr float SweptSegmentBoundingRadius(float segmentLength, float queryRadius)
{
    return segmentLength * 0.5f + (queryRadius > 0 ? queryRadius : 0);
}

// Squared distance from point (px,py,pz) to the segment b..e. Plain floats so it can be
// tested without the engine's vector types.
constexpr float SegmentPointDistance2(float bx, float by, float bz, float ex, float ey, float ez, float px, float py,
                                      float pz)
{
    const float dx = ex - bx, dy = ey - by, dz = ez - bz;
    const float len2 = dx * dx + dy * dy + dz * dz;
    float t = len2 > 0 ? ((px - bx) * dx + (py - by) * dy + (pz - bz) * dz) / len2 : 0;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const float qx = bx + dx * t - px, qy = by + dy * t - py, qz = bz + dz * t - pz;
    return qx * qx + qy * qy + qz * qz;
}

// May an object cell be skipped by a line query (Landscape::ObjectCollision)?
//
// Object::Intersect's own first test rejects every object whose centre is farther from
// the line than factor * Scale * BoundingSphere, factor <= 2 (2 when animated), before it
// does any work or recurses into proxies. `staticReach` bounds |pos - centre| + 2 * radius
// over a cell's static objects, so past that distance no object in the cell can survive
// it -- by the triangle inequality, dist(line, obj) >= dist(line, centre) - |obj - centre|.
// The slack absorbs rounding in the two distance computations.
constexpr bool LineQueryMaySkipCell(float distance2ToCentre, float staticReach)
{
    const float reach = staticReach + 0.5f;
    return distance2ToCentre > reach * reach;
}
}
