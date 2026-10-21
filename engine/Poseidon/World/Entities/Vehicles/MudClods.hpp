#pragma once

#include <Poseidon/World/Entities/Vehicles/Vehicle.hpp>
#include <Poseidon/World/Weather/MudClodPhysics.hpp>

namespace Poseidon
{
// One lump of wet soil thrown off a tyre (MudClodPhysics.hpp has the rules).
// A small lit 3D blob textured with the ground it was torn from and darkened
// wet: it tumbles through the air, lands, sits there and sinks back in.
class MudClod: public Vehicle
{
    typedef Vehicle base;

    MudClodState _state;

public:
    MudClod(LODShapeWithShadow* shape, const MudClodState& state);
    ~MudClod() override;

    void Simulate(float deltaT, SimulationImportance prec) override;

    USE_FAST_ALLOCATOR
};

// Throws `count` clods off one tyre. `contact`: where the tyre touches the
// ground; `forward`: the vehicle's facing; `velocity`: vehicle velocity (world).
// Respects MudClodMaxAlive and does nothing far from the camera.
void SpawnMudClods(Vector3Par contact, Vector3Par forward, Vector3Par velocity, float driveSign,
                   float groundSpeed, float spin, int count);

// Wheel helper: turns MudClodRate into whole clods over time (fractions carry).
struct MudClodEmitter
{
    float carry = 0.0f;
    int Take(float rate, float deltaT);
};
} // namespace Poseidon
