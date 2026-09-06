#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>

namespace Poseidon
{
class Object;

// WRL-004 (owner request 2026-09-06): an object that FALLS INTO water makes a splash.
//
// The interaction bridge already carries bullets, explosions, wading soldiers and ship wakes;
// a chair kicked off a pier, a crate dropped from a helicopter or a car driving off a jetty
// entered the water silently. This is one helper the per-step simulation of a moving object
// calls with where it was last step: when the object's bottom crosses the LOCAL water surface
// (the containing body's level, else the sea) going down faster than a gentle settle, it
// submits one Object interaction whose radius follows the object and whose strength follows
// the entry speed. Hysteresis is the caller's `prevY` (one event per crossing), and a body
// already under the surface last step never fires. Cheap: two float compares per step and a
// registry lookup only on the frames the object is actually near the surface.
//
// Returns true when an event was submitted (for tests and logs).
bool EmitWaterEntrySplash(const Object& object, Vector3Par previousPosition, Vector3Par velocity);

} // namespace Poseidon
