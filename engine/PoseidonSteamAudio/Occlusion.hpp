#pragma once

// Geometry occlusion for 3D sounds on the Steam Audio backend.
//
// Steam Audio's own occlusion needs a scene mesh; the island is 12.8 km of terrain plus
// tens of thousands of objects, so the line of sight is answered by the engine instead,
// on the main thread:
//   terrain   -- the heightmap sampled along the listener->source line; how far the
//                ground rises above the line (a hill, a ridge) sets a soft blockage;
//   buildings -- one fire-geometry sweep (Landscape::ObjectCollision) against STATIC
//                objects only: houses, walls. Soldiers and vehicles never occlude, so a
//                shooter's body or a tank's hull does not muffle its own sound; trees are
//                skipped (a trunk on the line is not a wall).
// The result is a per-band gain (low / mid / high, Steam Audio's three EQ bands), 1 = clear,
// applied by the voice's direct effect as transmission. The mixer smooths it.
//
// POSEIDON_STEAM_OCCLUSION=0 disables it (every voice clear, the behaviour before).

#include <Poseidon/Foundation/Math/Math3D.hpp>

namespace Poseidon
{
namespace SteamAudio
{

struct OcclusionResult
{
    float bands[3] = {1.f, 1.f, 1.f}; // low, mid, high gain; 1 = unoccluded
    float terrain = 0.f;              // 0..1 terrain blockage
    bool building = false;            // a static object's fire geometry is on the line
};

bool OcclusionEnabled();

// Main thread only (reads the landscape). `listener` and `source` in world coordinates.
OcclusionResult ComputeOcclusion(Vector3Par listener, Vector3Par source);

} // namespace SteamAudio
} // namespace Poseidon
