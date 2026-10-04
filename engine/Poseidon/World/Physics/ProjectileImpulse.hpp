#pragma once
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <cmath>

namespace Poseidon
{
// SI projectile momentum at the actual hit point. The caller applies the
// body's existing rigid-response coefficient once when consuming this impulse.
inline bool RifleProjectileImpulse(Vector3Par velocity, float projectileMass,
    Vector3Par impact, Vector3Par bodyCenter, Vector3& force, Vector3& torque)
{
    force=torque=VZero;
    if (!velocity.IsFinite() || !impact.IsFinite() || !bodyCenter.IsFinite() ||
        !std::isfinite(projectileMass) || projectileMass<=0) return false;
    const Vector3 momentum=velocity*projectileMass;
    const Vector3 lever=impact-bodyCenter;
    const Vector3 moment=lever.CrossProduct(momentum);
    if (!momentum.IsFinite() || !lever.IsFinite() || !moment.IsFinite() ||
        !std::isfinite(momentum.SquareSize()) || !std::isfinite(moment.SquareSize())) return false;
    force=momentum; torque=moment;
    return true;
}
}
