#pragma once

namespace Poseidon
{
class Object;

// Sinkhole W3: the swim constants, shared by the swim simulation (SoldierOldSwim.cpp), the AI's swim
// routes (AIUnitImpl.cpp), boarding and leaving boats (Ship.cpp, TransportCrew.cpp).
// The swim animations' water line (the animation-space height held on the surface; a living swimmer's
// follows his eyes, see SoldierOldSwim.cpp), and the water depths (mean level to bed) at which a wading
// man starts to swim and a swimmer stands up again.
constexpr float SwimWaterLine = 1.42f;
constexpr float SwimStartDepth = 1.45f;
constexpr float SwimStopDepth = 1.2f;

// The mean water level at x, z: a registered lake's or river's level inside it, else the (tidal) sea.
float SwimMeanWaterLevel(float x, float z);
// The water surface at x, z as a swimmer rides it: the waves (the CPU predictor; near the camera the
// surface as the Tidewater water draws it).
float SwimWaterSurfaceY(float x, float z);

// Sinkhole W3: the water under a camera that follows a swimmer. Returns false unless obj is a man who is
// swimming; then waterY is the wave surface at x, z (the CPU predictor the swimmer rides, or a lake's level).
// The player's camera is held above the flat mean sea everywhere else; on a swimmer that lifted the first
// person view out of his eyes into his head in every trough, and let a 3rd person view sink under the crests.
bool SwimmerCameraWater(const Object* obj, float x, float z, float& waterY);
} // namespace Poseidon
