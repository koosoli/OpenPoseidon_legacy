#pragma once

namespace Poseidon
{
class Object;

// Sinkhole W3: the water under a camera that follows a swimmer. Returns false unless obj is a man who is
// swimming; then waterY is the wave surface at x, z (the CPU predictor the swimmer rides, or a lake's level).
// The player's camera is held above the flat mean sea everywhere else; on a swimmer that lifted the first
// person view out of his eyes into his head in every trough, and let a 3rd person view sink under the crests.
bool SwimmerCameraWater(const Object* obj, float x, float z, float& waterY);
} // namespace Poseidon
