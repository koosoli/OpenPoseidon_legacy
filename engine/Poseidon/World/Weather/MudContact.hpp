#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/World/Weather/MudGroundAdmission.hpp>

namespace Poseidon
{
class Landscape;
class Object;

// Called only at actual animation foot-contact edges. Owns source, roadway,
// shore, snow and physical shelter admission, not rendering/collision mutation.
bool StampMudFootstep(const Landscape& land, Object& actor,
                      Vector3Par foot, Vector3Par direction);
} // namespace Poseidon
