#pragma once

namespace Poseidon
{
class LODShapeWithShadow;
}

namespace Poseidon::Model
{
struct Model;
}

namespace Poseidon::Dev
{
struct CaveMeshParams
{
    float width = 4.0f;
    float height = 3.0f;
    float length = 12.0f;
    float wallThickness = 0.4f;
    bool includeCeilingSelection = true;
};

/// Inner clearance: x=-width/2..width/2, y=0..height, z=0..length.
/// z=0 is open; positive z goes into the tunnel. The floor is exactly y=0.
/// Rejects non-finite/out-of-range dimensions instead of silently resizing them.
bool ValidCaveMeshParams(const CaveMeshParams& params);

/// CPU-only IR construction, with five separate closed convex slabs. Each call
/// has a distinct source name so the terrain-hole shape cache cannot reuse a
/// stale pointer/name pair. Invalid input returns an empty Model.
Model::Model BuildCaveModel(const CaveMeshParams& params);

/// Main-thread adapter/bank access. Caller takes ownership of the returned shape
/// (normally in Ref<LODShapeWithShadow>); invalid input/non-owner thread returns
/// nullptr. Does not place/register an Object, cut terrain or register Box3D.
/// Manual traversal is supported by Geometry + Roadway; no AI Paths are authored.
LODShapeWithShadow* BuildCaveMesh(const CaveMeshParams& params);
} // namespace Poseidon::Dev
