#pragma once
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <cmath>

namespace Poseidon
{
class Object;
class CollisionBuffer;

inline Vector3 CargoCross(Vector3Par a, Vector3Par b)
{
    return Vector3(a.Y()*b.Z()-a.Z()*b.Y(), a.Z()*b.X()-a.X()*b.Z(), a.X()*b.Y()-a.Y()*b.X());
}

// Two-sided finite segment test: interiors need back faces as well as front faces.
inline bool CargoTriangleHit(Vector3Par start, Vector3Par end, Vector3Par a,
                             Vector3Par b, Vector3Par c, float& t)
{
    const Vector3 d = end-start, e1 = b-a, e2 = c-a;
    const Vector3 p = CargoCross(d,e2);
    const float det = e1*p;
    if (!std::isfinite(det) || std::abs(det) < 1e-8f) return false;
    const Vector3 s = start-a;
    const float u = (s*p)/det;
    const Vector3 q = CargoCross(s,e1);
    const float v = (d*q)/det;
    const float hit = (e2*q)/det;
    if (!std::isfinite(hit) || u < 0 || v < 0 || u+v > 1 || hit < 0 || hit > 1) return false;
    t = hit;
    return true;
}

// Returns false when no usable visual mesh exists; callers retain solid geometry.
// Only the carrier shell is queried; crew continue through normal collision.
bool IntersectCargoSurface(Object& carrier, CollisionBuffer& result, Vector3Par start, Vector3Par end);
}
