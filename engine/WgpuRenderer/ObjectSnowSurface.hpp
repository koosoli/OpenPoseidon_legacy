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

struct VisualPose
{
    bool visual = false, loaded = false, deforms = false, animated = false;
    bool originalsValid = false;
    int vertices = 0;
};

// A stored retained receiver must not depend on a configured animation being
// momentarily neutral. Exact current-pose proof is additionally required for
// MAYANIMATE models, across all visual LODs and one shared vertex budget.
template <typename ReadVisual, typename ReadPair>
bool RetainedVisualsRigid(int levels, bool mayAnimate, bool configuredAnimations,
                         ReadVisual readVisual, ReadPair readPair)
{
    if (configuredAnimations || levels <= 0 || levels > MaxProofVertices) return false;
    int budget = MaxProofVertices;
    bool sawVisual = false;
    for (int level = 0; level < levels; ++level)
    {
        const VisualPose state = readVisual(level);
        if (!state.visual) continue;
        if (!state.loaded || state.deforms || state.animated || state.vertices < 0) return false;
        if (state.vertices == 0) continue;
        sawVisual = true;
        if (!mayAnimate) continue; // preserve the existing static-source fast path
        if (state.vertices > budget) return false;
        budget -= state.vertices;
        if (!CurrentSectionRigid(0, state.vertices, state.vertices, state.originalsValid,
            [&](int vertex) { return readPair(level, vertex); })) return false;
    }
    return sawVisual;
}
} // namespace Poseidon::ObjectSnowSurface
