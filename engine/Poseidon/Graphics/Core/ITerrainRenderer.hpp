#pragma once

#include <array>
#include <cstdint>

namespace Poseidon
{
class Scene;

// Producer-side terrain selection witness. No GPU completion or pixel claim.
struct TerrainDrawCoverage
{
    uint64_t frame = 0;
    uint32_t heightRevision = 0;
    int terrainRange = 0;
    float terrainGrid = 0;
    bool ready = false;
    bool background = false;
    bool pagedMaterials = false;
    uint32_t patches = 0;
    uint32_t outsideLegacyRect = 0;
    std::array<uint32_t, 16> levels{};
    std::array<float, 3> camera{};
    std::array<float, 4> legacyRect{};
    std::array<float, 2> treeBounds{};
    float cameraFar = 0;
    float farthestPatch = 0;
};

class ITerrainRenderer
{
  public:
    virtual ~ITerrainRenderer() = default;

    // Emit the terrain covering land-cell rectangle [xBeg,xEnd) x [zBeg,zEnd) for
    // this frame. Called once per frame from Landscape::DrawGround (opaque layer).
    virtual void DrawTerrain(Scene& scene, int xBeg, int zBeg, int xEnd, int zEnd) = 0;
    virtual bool GetDrawCoverage(TerrainDrawCoverage&) const { return false; }
};

} // namespace Poseidon
