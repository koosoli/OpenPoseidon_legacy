#pragma once

#include <cstdint>
#include <vector>

namespace Poseidon
{

// WRL-003 — simulation-owned water bodies other than the global sea.
//
// Stock OFP/CWA worlds carry exactly one body of water: the tidal sea plane
// (Landscape::GetSeaLevel). Imported worlds carry ponds and rivers as model surfaces with no
// elevation, extent or flow metadata, so until an importer supplies that, bodies enter this
// registry from fixtures (the Water tab's synthetic lake / river) and from any future loader.
//
// This is the CPU-side contract the plan's section 3.2 asks for, in its minimal form:
//   - stable identity and a generation counter (renderer and physics can detect reloads);
//   - kind (ocean is never registered here: "no body" means the global sea);
//   - horizontal bounds (an axis-aligned box plus an ellipse inside it, the representation the
//     synthetic fixtures need; a polygon can be added without changing the callers);
//   - mean surface elevation, with a linear gradient for a sloping river reach;
//   - bounded wave character (an amplitude scale for the CPU predictor) and a documented flow.
//
// Ownership: the simulation owns this. Rendering READS it (WaterWgpu emits nodes per body and
// picks the camera's body for the underwater compositor); it never decides body availability
// from visibility, and no GPU readback feeds it. Queries are plain arithmetic so they work
// headless and off-screen. Overlaps resolve deterministically: the smaller body wins, ties by
// lower id — a pond inside a lake is the pond.
enum class WaterBodyKind : uint8_t
{
    Lake = 1,
    River = 2,
};

// Authored optical/character profile (plan section 3.3). The renderer maps these onto
// shallow/deep swatch and clarity multipliers; the CPU side only carries the wave scale.
enum class WaterBodyProfile : uint8_t
{
    ClearLake = 0,
    TurbidLake = 1,
    SlowRiver = 2,
    FastShallowRiver = 3,
};

struct WaterBody
{
    uint32_t id = 0;
    WaterBodyKind kind = WaterBodyKind::Lake;
    WaterBodyProfile profile = WaterBodyProfile::ClearLake;
    // Horizontal extent: ellipse centred at (centreX, centreZ) with semi-axes (radiusX, radiusZ).
    // The bounding box is derived from it; Contains() tests the ellipse.
    float centreX = 0.0f;
    float centreZ = 0.0f;
    float radiusX = 1.0f;
    float radiusZ = 1.0f;
    // Mean surface elevation at the centre, and its gradient (metres per metre) for a sloping
    // river reach. A lake has a zero gradient.
    float level = 0.0f;
    float gradientX = 0.0f;
    float gradientZ = 0.0f;
    // Amplitude scale applied to the CPU wave predictor inside this body (1 = ocean).
    // Lakes are sheltered: small ripples, no swell.
    float waveScale = 0.08f;
    // Visual flow: world-xz direction and speed (m/s). This is texture/foam advection only; it
    // applies no force (plan section 3.6: physical current is a separate, unauthorised change).
    float flowX = 0.0f;
    float flowZ = 0.0f;
    float flowSpeed = 0.0f;
    // Yaw of the ellipse's local x axis in the world xz plane (radians; 0 = world +x). A river
    // reach follows its heading; lakes leave it at 0.
    float rotation = 0.0f;
    // Bed-following (rivers): when > 0 the surface is the TERRAIN height along the reach's
    // centreline plus this depth, instead of the plane above. A plane cannot follow a valley
    // floor; the centreline profile can, and the CPU and GPU sample the same heightfield.
    // 0 = plane mode (lakes).
    float bedDepth = 0.0f;

    // World-axis half extents of the rotated ellipse (its bounding box).
    float ExtentX() const;
    float ExtentZ() const;
    float MinX() const { return centreX - ExtentX(); }
    float MaxX() const { return centreX + ExtentX(); }
    float MinZ() const { return centreZ - ExtentZ(); }
    float MaxZ() const { return centreZ + ExtentZ(); }
    // World xz -> the ellipse's local frame (unrotated, centred).
    void ToLocal(float x, float z, float& lx, float& lz) const;
    float Area() const;
    bool Contains(float x, float z) const;
    // Mean surface elevation at a world XZ (the plane, before waves).
    float SurfaceLevelAt(float x, float z) const;
};

class WaterBodyRegistry
{
  public:
    // Returns the assigned id. Ids are never reused within a generation.
    uint32_t Add(WaterBody body);
    bool Remove(uint32_t id);
    void Clear();
    // Bumps on every mutation; consumers that cache per-body resources compare it.
    uint32_t Generation() const { return _generation; }
    const std::vector<WaterBody>& Bodies() const { return _bodies; }
    bool Empty() const { return _bodies.empty(); }
    // The body containing (x, z), or nullptr for "the global sea / dry land". Deterministic on
    // overlap: smallest area wins, then lowest id.
    const WaterBody* Find(float x, float z) const;
    const WaterBody* FindById(uint32_t id) const;

  private:
    std::vector<WaterBody> _bodies;
    uint32_t _nextId = 1;
    uint32_t _generation = 0;
};

// Process-wide registry (one world at a time). Cleared by the world on map change.
WaterBodyRegistry& GetWaterBodies();

} // namespace Poseidon
