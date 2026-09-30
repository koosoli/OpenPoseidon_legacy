#pragma once

#include <array>
#include <cmath>

namespace Poseidon::ObjectSnowSurface
{
// A bound on work per drawn section run, not permission to scan a whole vehicle.
inline constexpr int MaxProofVertices = 16384;

struct PosePair
{
    std::array<float, 6> current; // position xyz, normal xyz
    std::array<float, 6> original;
};

inline bool ConformAllowed(float mode, bool hasKeep, bool hasPinned)
{
    // Keep changes elevation above the ground; On pins a surface onto it.
    // Preserve the existing rigid path, reject forest-plane/unknown modes.
    return mode == 0.0f || (mode == 2.0f && hasKeep && !hasPinned);
}

inline bool CoverPossible(bool enabled, float deposit, bool snowlineEnabled,
                          float snowlineHeight, float snowlineRange, float snowlineDepth,
                          float worldUpperY)
{
    if (enabled && std::isfinite(deposit) && deposit > 0.0f) return true;
    return snowlineEnabled && std::isfinite(snowlineHeight) &&
        std::isfinite(snowlineRange) && snowlineRange > 0.0f &&
        std::isfinite(snowlineDepth) && snowlineDepth > 0.0f &&
        std::isfinite(worldUpperY) && worldUpperY > snowlineHeight;
}

// This proves only the currently drawn pose. An articulated selection in its
// original pose can pass; it loses admission as soon as positions/normals change.
// Read only a validated bounded span. Missing original data fails closed.
template <typename ReadPair>
bool CurrentSectionRigid(int begin, int end, int vertexCount,
                         bool originalsValid, ReadPair read)
{
    if (!originalsValid || begin < 0 || end <= begin || end > vertexCount ||
        end - begin > MaxProofVertices) return false;
    for (int vertex = begin; vertex < end; ++vertex)
    {
        const PosePair pair = read(vertex);
        for (unsigned component = 0; component < pair.current.size(); ++component)
            if (!std::isfinite(pair.current[component]) ||
                !std::isfinite(pair.original[component]) ||
                pair.current[component] != pair.original[component]) return false;
    }
    return true;
}
} // namespace Poseidon::ObjectSnowSurface
