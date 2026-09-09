#pragma once
#include <Poseidon/World/Physics/PhysicsTypes.hpp>
#include <algorithm>
#include <cmath>

namespace Poseidon::Dev
{
// Conservative upright player capsule against an oriented block. Expanding the
// slabs also sweeps fast movement, rather than detecting penetration a tick late.
inline bool SweepShowcaseBlock(const Physics::ProbeSample& block, Vector3Par from, Vector3Par to,
                               float radius, float halfSegment, float& fraction, Vector3& normal)
{
    if (block.shape != Physics::ProbeShape::Box) return false;
    const Vector3 axes[] = {block.axisX, block.axisY, block.axisZ};
    const Vector3 offset = from - block.position;
    const Vector3 travel = to - from;
    float enter = 0.0f, leave = fraction;
    Vector3 entryNormal = VZero;
    bool outside = false;
    for (int i = 0; i < 3; ++i)
    {
        const float extent = block.halfExtents[i] + radius + halfSegment * std::abs(axes[i].Y());
        const float p = offset.DotProduct(axes[i]);
        const float d = travel.DotProduct(axes[i]);
        outside |= std::abs(p) >= extent;
        if (std::abs(d) < 1e-7f)
        {
            if (std::abs(p) >= extent) return false;
            continue;
        }
        float near = (-extent-p)/d, far = (extent-p)/d;
        Vector3 n = -axes[i];
        if (near > far) { std::swap(near, far); n = axes[i]; }
        if (near >= enter) { enter = near; entryNormal = n; }
        leave = std::min(leave, far);
        if (enter > leave) return false;
    }
    // Do not trap a player teleported inside a block: allow movement out.
    if (!outside || entryNormal.SquareSize() < 0.5f || leave < 0 || enter >= fraction) return false;
    fraction = enter;
    normal = entryNormal;
    return true;
}

inline Vector3 ShowcaseBlastImpulse(Vector3Par delta, float hit, float range)
{
    const float distance = delta.Size();
    if (!(range > 0) || !(hit > 0) || distance >= range*4) return VZero;
    const float attenuation = std::min(1.0f, range*range/std::max(distance*distance, 0.01f));
    const float edge = std::min(1.0f, (range*4-distance)/range);
    const Vector3 direction = distance > 0.01f ? delta/distance : VUp;
    return direction * (std::min(hit*2.0f, 400.0f) * attenuation * edge);
}
}
