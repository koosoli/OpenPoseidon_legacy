// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What a projectile does once it is under water.
//
// Water is ~800x denser than air, so the same v^2 drag law that takes a rifle round
// hundreds of metres to slow takes it centimetres here. Fired into water, high
// velocity rifle rounds also yaw and break up almost at once, which multiplies the
// drag again: measured rounds are spent within about a metre, slower pistol rounds
// within two to three. A swimmer half a metre down can be hurt; two metres down he
// is safe from small arms. A grenade sinks and goes off on the bottom.
//
// The drag here is integrated EXACTLY. dv/dt = -k v^2 has the closed form
// v(t) = v0 / (1 + k v0 t), x(t) = ln(1 + k v0 t) / k; explicit Euler at
// k*v*dt ~ 40 (a 900 m/s round at 60 Hz) would reverse the bullet in one step.
//
// ON THE NUMBERS: drag coefficients are open estimates, chosen to land on the
// published water-penetration behaviour above; not to be quoted as data.

#include <algorithm>
#include <cmath>

namespace Poseidon::Underwater
{
constexpr float WaterDensity = 1000.0f; // kg/m^3
/// Below this a small-arms round is spent: it sinks harmlessly and is retired.
constexpr float RetireSpeed = 40.0f;
/// Rounds entering faster than this yaw and fragment (rifle); slower ones stay whole (pistol).
constexpr float FragmentSpeed = 600.0f;

inline float DragCoefficient(float entrySpeed)
{
    return entrySpeed > FragmentSpeed ? 0.9f : 0.25f;
}

/// k in a = -k v^2, 1/m, from the round's mass (kg) and calibre (m).
inline float DragK(float mass, float calibre, float entrySpeed)
{
    const float radius = std::max(calibre, 0.002f) * 0.5f;
    const float area = 3.14159265f * radius * radius;
    return 0.5f * WaterDensity * DragCoefficient(entrySpeed) * area / std::max(mass, 1e-4f);
}

/// Exact v^2-drag step: distance covered and the speed after `dt` seconds.
inline void Advance(float v0, float k, float dt, float& distance, float& v1)
{
    if (!(v0 > 0.0f) || !(k > 0.0f) || !(dt > 0.0f))
    {
        distance = 0.0f;
        v1 = std::max(v0, 0.0f);
        return;
    }
    const float s = 1.0f + k * v0 * dt;
    v1 = v0 / s;
    distance = std::log(s) / k;
}

/// Distance at which a round entering at v0 drops to `speed` (v = v0 exp(-k x)).
inline float DistanceToSpeed(float v0, float k, float speed)
{
    return v0 > speed && k > 0.0f ? std::log(v0 / speed) / k : 0.0f;
}

/// Share of its entry energy a round still carries: scales its hit when it strikes.
inline float EnergyScale(float speed, float entrySpeed)
{
    if (!(entrySpeed > 0.0f)) return 0.0f;
    const float r = speed / entrySpeed;
    return std::clamp(r * r, 0.0f, 1.0f);
}

// A hand grenade under water: ~0.4 kg, ~6 cm across, about 2.5x as dense as water.
constexpr float GrenadeDragK = 0.5f * WaterDensity * 0.5f * 3.14159265f * 0.03f * 0.03f / 0.4f; // ~0.88 1/m
constexpr float GrenadeSinkAccel = 9.81f * (1.0f - 1.0f / 2.5f);                             // net of buoyancy

/// One stable step of a sinking body: implicit quadratic drag, then net weight.
/// Velocity components are (x, y, z) with y up.
inline void SinkStep(float& vx, float& vy, float& vz, float k, float sinkAccel, float dt)
{
    const float speed = std::sqrt(vx * vx + vy * vy + vz * vz);
    const float damp = 1.0f / (1.0f + k * speed * dt);
    vx *= damp;
    vy *= damp;
    vz *= damp;
    vy -= sinkAccel * dt;
}

/// How an explosion below the surface treats a target. `blastDepth` is metres of
/// water above the charge; returns false for a blast in air (no change).
struct BlastShaping
{
    float hitScale = 1.0f;   // multiplies the indirect hit
    float rangeScale = 1.0f; // multiplies the indirect hit range
};
inline bool ShapeUnderwaterBlast(float blastDepth, bool targetSubmerged, BlastShaping& out)
{
    if (!(blastDepth > 0.3f)) return false;
    if (targetSubmerged)
    {
        // Water is nearly incompressible: the shock reaches much further than in air.
        out.hitScale = 1.0f;
        out.rangeScale = 2.0f;
    }
    else
    {
        // Above the surface only the spout and a weakened pressure pulse arrive.
        out.hitScale = 0.2f;
        out.rangeScale = 1.0f;
    }
    return true;
}

bool Enabled();
} // namespace Poseidon::Underwater
