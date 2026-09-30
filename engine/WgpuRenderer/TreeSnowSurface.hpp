#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace Poseidon::TreeSnowSurface
{
// Reserved conform lanes are meaningful only with this positive source proof.
// Bounds belong to the loaded owner model, shared by every visual LOD; never
// derive them from the current leaf section, camera or wind-displaced vertices.
struct CrownBounds { float minY = 0, inverseHeight = 0; };
inline constexpr unsigned MaxCachedOwners = 4096;
struct ProbeBudget
{
    uint64_t generation = 0;
    unsigned remaining = 0;
    void BeginFrame(uint64_t frame)
    {
        if (frame == generation) return;
        generation = frame;
        remaining = frame ? 8 : 0;
    }
    bool Take() { if (!remaining) return false; --remaining; return true; }
};
inline bool ReuseProof(bool sameIdentityAndTransform, double ageSeconds)
{
    return sameIdentityAndTransform && std::isfinite(ageSeconds) &&
        ageSeconds >= 0 && ageSeconds < 1.0;
}
inline bool SameOwner(uint32_t storedBirth, uint32_t currentBirth,
                      const void* storedShape, const void* currentShape,
                      const float* storedTransform, const float* currentTransform)
{
    return storedBirth == currentBirth && storedShape == currentShape &&
        std::memcmp(storedTransform, currentTransform, 16 * sizeof(float)) == 0;
}
enum class RoofGeometry { Fire, View };
// Actual caller supplies the SAME ignored owner/origin/end to both geometry
// queries. View fallback admits bullet-permeable roofs such as stock CampEastC.
template<class Owner, class Point, class Query>
bool ProbeOpen(Owner ignore, const Point& origin, const Point& end, Query query)
{
    if (query(ignore, origin, end, RoofGeometry::Fire)) return false;
    return !query(ignore, origin, end, RoofGeometry::View);
}
inline CrownBounds AdmitBounds(float minY, float maxY)
{
    const float height = maxY - minY;
    if (!std::isfinite(minY) || !std::isfinite(maxY) ||
        !std::isfinite(height) || height <= 0.001f) return {};
    const float inverse = 1.0f / height;
    return std::isfinite(inverse) ? CrownBounds{minY, inverse} : CrownBounds{};
}
inline bool OwnerAllowed(bool primary, bool isStatic, bool intact,
                         bool individualTree, bool ownsMesh, bool unposed,
                         bool conformAllowed)
{
    return primary && isStatic && intact && individualTree && ownsMesh &&
        unposed && conformAllowed;
}
inline bool LeafSectionAllowed(bool cutout, bool opaqueDepthWriting,
                               bool unposed, bool ordinarySurface)
{
    return cutout && opaqueDepthWriting && unposed && ordinarySurface;
}
}
