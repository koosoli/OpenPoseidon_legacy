#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/World/Weather/MudGroundAdmission.hpp>

namespace Poseidon
{
class Landscape;
class Object;

// Called only at actual animation foot-contact edges. Owns source, roadway,
// shore, snow and physical shelter admission, then pressure compression of the
// persistent signed height field. Terrain consumers own geometry integration.
bool StampMudFootstep(const Landscape& land, Object& actor,
                      Vector3Par foot, Vector3Par direction);
// Prototype wheel anchor. Returns admission even when a rut is saturated.
// Caller must prove actual wheel/ground contact and handle distance sampling.
bool StampMudWheelContact(const Landscape& land, Object& actor, Vector3Par point);
} // namespace Poseidon
