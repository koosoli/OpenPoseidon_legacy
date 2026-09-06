#include <Poseidon/World/Simulation/WaterEntrySplash.hpp>

#include <Poseidon/Graphics/Rendering/WaterInteractionBridge.hpp>
#include <Poseidon/World/Scene/Object.hpp>
#include <Poseidon/World/Terrain/Landscape.hpp>
#include <Poseidon/World/Terrain/WaterBodies.hpp>

#include <algorithm>
#include <cmath>

namespace Poseidon
{

bool EmitWaterEntrySplash(const Object& object, Vector3Par previousPosition, Vector3Par velocity)
{
    if (GLandscape == nullptr)
    {
        return false;
    }
    const Vector3 position = object.WorldPosition();
    // Only a falling object can enter; a rising or drifting one is a wake, not a splash.
    constexpr float minEntrySpeed = 0.8f; // m/s downward; slower than that is a settle
    if (velocity.Y() > -minEntrySpeed)
    {
        return false;
    }
    // Local surface: the containing body's mean level, else the (tidal) sea.
    float level = GLandscape->GetSeaLevel();
    if (const WaterBody* body = GetWaterBodies().Find(position.X(), position.Z()))
    {
        level = body->SurfaceLevelAt(position.X(), position.Z());
    }
    // The object's bottom, not its centre: a crate whose centre is a metre above the water
    // has already splashed when its underside touches.
    const float radius = std::clamp(object.GetRadius(), 0.2f, 6.0f);
    const float bottomNow = position.Y() - radius * 0.5f;
    const float bottomBefore = previousPosition.Y() - radius * 0.5f;
    if (!(bottomBefore > level && bottomNow <= level))
    {
        return false;
    }
    // Dry ground under the surface line (a road below sea level on a coast) is not water.
    if (GLandscape->SurfaceY(position.X(), position.Z()) >= level)
    {
        return false;
    }
    const float entrySpeed = -velocity.Y();
    HydroWaterInteractionEvent event{};
    event.positionRadius[0] = position.X();
    event.positionRadius[1] = position.Z();
    event.positionRadius[2] = std::clamp(radius * 1.4f, 0.6f, 6.0f);
    // Strength grows with the entry speed; a 5 m/s drop is a bullet-class splash (3.8), a
    // 15 m/s fall from a helicopter approaches an explosion (4.5).
    event.positionRadius[3] = std::clamp(1.5f + entrySpeed * 0.3f, 1.5f, 5.0f);
    event.velocityKind[0] = velocity.X();
    event.velocityKind[1] = velocity.Z();
    event.velocityKind[2] = -entrySpeed;
    event.velocityKind[3] = HydroWaterInteractionObject;
    event.timeLifeFoamMass[1] = std::clamp(1.2f + radius * 0.4f, 1.2f, 3.5f); // lifetime
    event.timeLifeFoamMass[2] = std::clamp(0.4f + entrySpeed * 0.06f, 0.4f, 1.5f); // foam density
    event.timeLifeFoamMass[3] = radius;
    const float horizontal = std::sqrt(velocity.X() * velocity.X() + velocity.Z() * velocity.Z());
    event.directionDepthFlags[0] = horizontal > 1e-3f ? velocity.X() / horizontal : 0.0f;
    event.directionDepthFlags[1] = horizontal > 1e-3f ? velocity.Z() / horizontal : 0.0f;
    event.directionDepthFlags[2] = level - bottomNow;
    event.directionDepthFlags[3] =
        HydroWaterInteractionPendingImpulse | (radius > 2.0f ? HydroWaterInteractionLargeBody : 0u);
    SubmitWaterInteraction(event);
    return true;
}

} // namespace Poseidon
