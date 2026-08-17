#include <Poseidon/World/Weather/AirflowField.hpp>

#include <Poseidon/World/Weather/WindModel.hpp>
#include <Poseidon/World/World.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Entities/Vehicles/Air/Helicopter.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Poseidon
{
namespace
{

/// Hermite smoothstep, clamped. Used for the edge of the rotor column: a hard
/// cutoff makes a particle crossing the edge visibly snap.
inline float SmoothStep(float edge0, float edge1, float x)
{
    if (edge1 <= edge0)
    {
        return x < edge0 ? 0.0f : 1.0f;
    }
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Rotor wake shape constants. These are visual tuning, not aerodynamics; they
// were chosen so that a Huey at a 10 m hover flattens smoke over roughly the
// area the grass shader already flattens, which is the one existing reference
// the look has to agree with.

/// How fast the wake cone widens with depth below the disc. A real wake
/// contracts first and then spreads; over the 0-60 m range that matters here a
/// straight taper is indistinguishable and much cheaper.
constexpr float kConeSpread = 0.12f;
/// Depth below the disc at which the column has lost half its speed.
constexpr float kAxialHalfDepth = 22.0f;
/// Height above ground within which the column has fully turned into an
/// outward-running ground jet.
constexpr float kJetHeight = 4.0f;
/// Outward jet speed as a fraction of the column speed that fed it.
constexpr float kJetFraction = 0.85f;
/// Distance beyond the cone edge, in cone radii, over which the jet dies.
constexpr float kJetDecayRadii = 2.5f;

} // namespace

Vector3 EvaluateRotorWash(const RotorWash& wash, Vector3Par pos)
{
    if (wash.strength <= 0.0f || wash.discRadius <= 0.0f)
    {
        return VZero;
    }

    const float dx = pos.X() - wash.position.X();
    const float dz = pos.Z() - wash.position.Z();
    const float r = std::sqrt(dx * dx + dz * dz);

    // Depth below the disc, positive downward. Above the disc there is an inflow
    // rather than a wake; it is weak, it is upward, and simulating it would make
    // smoke rise into the blades. Ignored deliberately.
    const float depth = wash.position.Y() - pos.Y();
    if (depth < -wash.discRadius)
    {
        return VZero;
    }
    const float clampedDepth = std::max(depth, 0.0f);

    // The column loses speed with depth. 1/(1+d/half) rather than an exp so the
    // tail is longer: a wake is still felt well past the point where it has
    // halved, and an exponential cuts it off too tidily.
    const float axialFall = 1.0f / (1.0f + clampedDepth / kAxialHalfDepth);
    const float cone = wash.discRadius * (1.0f + kConeSpread * clampedDepth / wash.discRadius);

    // Flat across the core, soft at the rim. A rotor wake really does have a
    // fairly sharp edge, so the falloff starts at 70% of the radius.
    const float core = 1.0f - SmoothStep(0.70f * cone, cone, r);

    // How much of the column has already turned into a ground jet at this height.
    const float heightAboveGround = pos.Y() - wash.groundY;
    const float jetBlend = 1.0f - std::clamp(heightAboveGround / kJetHeight, 0.0f, 1.0f);

    const float columnSpeed = wash.strength * axialFall;

    Vector3 velocity(VZero);

    // Vertical component: what has not yet been turned outward.
    velocity[1] = -columnSpeed * core * (1.0f - jetBlend);

    // Radial component: the ground jet. It exists outside the cone as well as
    // inside it — that outward run along the ground is the whole visible
    // signature of a helicopter over dust or smoke — so it is not gated on
    // `core`. It decays with distance past the cone edge.
    if (jetBlend > 0.0f && r > 1e-3f)
    {
        // Inside the cone the jet builds from zero at the axis (there is nothing
        // to push outward on the centreline); outside it decays.
        const float inside = std::min(r / cone, 1.0f);
        const float outside = 1.0f / (1.0f + std::max(r - cone, 0.0f) / (kJetDecayRadii * cone));
        const float jetSpeed = columnSpeed * kJetFraction * jetBlend * inside * outside;

        velocity[0] = jetSpeed * (dx / r);
        velocity[2] = jetSpeed * (dz / r);
    }

    return velocity;
}

void AirflowField::Reset()
{
    _count = 0;
}

void AirflowField::Update(Vector3Par focus)
{
    _count = 0;

    if (GWorld == nullptr || GLandscape == nullptr)
    {
        return;
    }

    // Nearest-first insertion into a fixed array. No allocation, no sort, and
    // the cost is O(vehicles * kMaxRotorWash) with kMaxRotorWash = 4.
    float distance2[kMaxRotorWash];
    for (std::size_t i = 0; i < kMaxRotorWash; ++i)
    {
        distance2[i] = std::numeric_limits<float>::infinity();
    }

    const auto consider = [&](const Helicopter* helicopter)
    {
        if (helicopter == nullptr)
        {
            return;
        }

        // Rotor state, not Airborne(): the wash starts while the skids are still
        // down, which is exactly when it is most visible, and rotor inertia
        // keeps it alive after the engine is cut. Same test the grass shader's
        // feed uses, so the two effects begin and end together.
        const float rotorSpeed = std::clamp(helicopter->RotorSpeed(), 0.0f, 1.0f);
        if (rotorSpeed <= 0.02f)
        {
            return;
        }

        const Vector3 pos = helicopter->Position();
        const float groundY = GLandscape->SurfaceY(pos.X(), pos.Z());
        const float height = pos.Y() - groundY;
        if (height > 80.0f)
        {
            // Above this the wake has spread and slowed past the point where it
            // does anything a viewer would notice, and the cost is real.
            return;
        }

        RotorWash wash;
        wash.position = pos;
        // The bounding sphere covers the whole airframe; a main rotor disc is
        // most of but not all of that. 0.8 puts a Huey near its real 7.3 m.
        wash.discRadius = std::max(helicopter->GetRadius() * 0.80f, 3.0f);
        // Downwash speed scales with rotor speed squared (thrust goes as omega
        // squared, induced velocity as its root — but the visible effect tracks
        // thrust, and the square is what the grass feed already uses).
        wash.strength = 14.0f * rotorSpeed * rotorSpeed;
        wash.groundY = groundY;

        const float d2 = (pos - focus).SquareSizeXZ();

        // Displace the farthest current entry if this one is closer.
        std::size_t slot = 0;
        for (std::size_t j = 1; j < kMaxRotorWash; ++j)
        {
            if (distance2[j] > distance2[slot])
            {
                slot = j;
            }
        }
        if (d2 < distance2[slot])
        {
            distance2[slot] = d2;
            _rotors[slot] = wash;
        }
    };

    for (int i = 0; i < GWorld->NVehicles(); ++i)
    {
        consider(dynamic_cast<const Helicopter*>(GWorld->GetVehicle(i)));
    }

    // Compact: slots that were never filled still hold infinity.
    for (std::size_t i = 0; i < kMaxRotorWash; ++i)
    {
        if (distance2[i] < std::numeric_limits<float>::infinity())
        {
            _rotors[_count++] = _rotors[i];
        }
    }
}

Vector3 AirflowField::SampleLocal(Vector3Par pos) const
{
    if (!_localEnabled || _count == 0)
    {
        return VZero;
    }

    Vector3 local(VZero);
    for (std::size_t i = 0; i < _count; ++i)
    {
        local += EvaluateRotorWash(_rotors[i], pos);
    }
    return local * _localScale;
}

Vector3 AirflowField::Sample(Vector3Par pos) const
{
    Vector3 total(VZero);

    if (GWind.IsActive())
    {
        const WindSample& wind = GWind.Sample();
        total[0] = wind.velocityX;
        total[2] = wind.velocityZ;
    }

    return total + SampleLocal(pos);
}

AirflowField& GAirflowField()
{
    static AirflowField instance;
    return instance;
}

} // namespace Poseidon
