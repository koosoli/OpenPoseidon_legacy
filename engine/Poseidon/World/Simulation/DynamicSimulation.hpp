#pragma once

// Arma 3's dynamic simulation: an entity a script marked for it (enableDynamicSimulation) is
// frozen -- neither simulated nor thought for -- while no player or camera is within the
// distance of its kind. Old Man turns it on for every group and object at boot and leaves
// a few hundred units across Tanoa; without it every one of them thought every tick.

namespace Poseidon
{
class World;

namespace DynSim
{
enum class Kind
{
    Group,        // a unit, by its group leader's position
    Vehicle,      // a vehicle with crew
    EmptyVehicle, // a vehicle without
    Prop,         // anything else
    Count
};

// "Group", "Vehicle", "EmptyVehicle", "Prop" (any case); false for another name
bool KindFromName(const char* name, Kind& kind);

void SetSystemEnabled(bool enabled);
bool SystemEnabled();
void SetDistance(Kind kind, float metres);
float Distance(Kind kind);
// "IsMoving": a moving entity's distance is multiplied by this
void SetMovingCoef(float coef);
float MovingCoef();

// back to Arma 3's defaults, for a new mission
void Reset();

// Freezes and wakes the marked entities; cheap between its updates (twice a second).
void Update(World& world);
} // namespace DynSim
} // namespace Poseidon
