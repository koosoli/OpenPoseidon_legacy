#pragma once
#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <Poseidon/World/Weather/SandGroundAdmission.hpp>

namespace Poseidon
{
class Landscape;
class Object;
// Actual animation boot edge only; all source/support checks precede stamping.
bool StampSandFootstep(const Landscape& land, Object& actor,
                       Vector3Par foot, Vector3Par direction);
} // namespace Poseidon
