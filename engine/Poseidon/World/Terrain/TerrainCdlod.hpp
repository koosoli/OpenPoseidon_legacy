// Terrain CDLOD quadtree selection — pure, no engine dependencies.
#pragma once

#include <algorithm>
#include <vector>

namespace Poseidon
{
struct CdlodNode
{
    float originX, originZ;
    float size;
    float minY, maxY;
    int level;
    int child[4]; // child[0] < 0 marks a leaf
};

struct CdlodSelection
{
    float originX, originZ;
    float size;
    int level;
    float morphStart, morphEnd;
};

// Refine only the close-up snow window, not the entire authored heightfield.
// Children partition the parent exactly; no overlapping overlay or depth bias.
template <typename EmitFn>
void RefineCdlodSnow(const CdlodSelection& node, float x, float z, float radius, EmitFn&& emit)
{
    if (node.size <= 4.0f || node.originX >= x + radius || node.originZ >= z + radius ||
        node.originX + node.size <= x - radius || node.originZ + node.size <= z - radius)
    {
        emit(node);
        return;
    }
    for (int child = 0; child < 4; ++child)
    {
        auto patch = node;
        patch.size *= 0.5f;
        patch.originX += (child & 1) * patch.size;
        patch.originZ += (child >> 1) * patch.size;
        // The normal LOD ladder has no levels below its authored leaf. These
        // geometric samples must not snap back to that coarse lattice.
        patch.morphStart = 1.0e10f;
        patch.morphEnd = 2.0e10f;
        RefineCdlodSnow(patch, x, z, radius, emit);
    }
}

// ranges[L] is the distance out to which level L is the coarsest acceptable detail.
inline void ComputeCdlodRanges(float baseRange, float ratio, int numLevels, std::vector<float>& ranges)
{
    ranges.resize(numLevels);
    float r = baseRange;
    for (int i = 0; i < numLevels; i++)
    {
        ranges[i] = r;
        r *= ratio;
    }
}

inline float CdlodNodeDistanceSq(const CdlodNode& n, float px, float py, float pz)
{
    const float maxX = n.originX + n.size;
    const float maxZ = n.originZ + n.size;
    const float dx = px < n.originX ? n.originX - px : (px > maxX ? px - maxX : 0.0f);
    const float dy = py < n.minY ? n.minY - py : (py > n.maxY ? py - n.maxY : 0.0f);
    const float dz = pz < n.originZ ? n.originZ - pz : (pz > maxZ ? pz - maxZ : 0.0f);
    return dx * dx + dy * dy + dz * dz;
}

// Distances over which a patch at `level` morphs toward its parent grid; fully
// morphed at ranges[level].
inline void CdlodMorphBand(const std::vector<float>& ranges, int level, float morphRegion, float& morphStart,
                           float& morphEnd)
{
    const float end = ranges[level];
    const float prev = level > 0 ? ranges[level - 1] : 0.0f;
    morphStart = end - (end - prev) * morphRegion;
    morphEnd = end;
}

template <typename EmitFn>
inline void EmitCdlodNode(const CdlodNode& n, int level, const std::vector<float>& ranges, float morphRegion,
                          EmitFn&& emit)
{
    float ms = 0.0f, me = 0.0f;
    CdlodMorphBand(ranges, level, morphRegion, ms, me);
    emit(CdlodSelection{n.originX, n.originZ, n.size, level, ms, me});
}

// Descends the quadtree, choosing each node's level by distance and frustum.
// `visible` frustum-tests a node; `emit` receives each node to draw. Returns
// false when the node is beyond ranges[lodLevel], so the caller draws the area
// coarser instead.
template <typename VisibleFn, typename EmitFn>
bool SelectCdlod(const std::vector<CdlodNode>& nodes, int idx, int lodLevel, float camX, float camY, float camZ,
                 const std::vector<float>& ranges, float morphRegion, VisibleFn&& visible, EmitFn&& emit)
{
    const CdlodNode& n = nodes[idx];
    const float distSq = CdlodNodeDistanceSq(n, camX, camY, camZ);
    if (distSq > ranges[lodLevel] * ranges[lodLevel])
    {
        return false;
    }
    if (!visible(n))
    {
        return true;
    }
    if (lodLevel == 0 || n.child[0] < 0)
    {
        EmitCdlodNode(n, lodLevel, ranges, morphRegion, emit);
        return true;
    }
    if (distSq > ranges[lodLevel - 1] * ranges[lodLevel - 1])
    {
        EmitCdlodNode(n, lodLevel, ranges, morphRegion, emit);
        return true;
    }
    for (int c = 0; c < 4; c++)
    {
        const int childIdx = n.child[c];
        if (!SelectCdlod(nodes, childIdx, lodLevel - 1, camX, camY, camZ, ranges, morphRegion, visible, emit) &&
            visible(nodes[childIdx]))
        {
            // At the child's own level, not the parent's: its morph band must match
            // its geometry level or it draws un-morphed and cracks at coarse edges.
            EmitCdlodNode(nodes[childIdx], lodLevel - 1, ranges, morphRegion, emit);
        }
    }
    return true;
}

// Flat ocean roots need no stored descendants or terrain scan. Select lazily,
// with exactly the same distance/morph policy as the terrain-backed tree.
template <typename VisibleFn, typename EmitFn>
bool SelectFlatCdlod(const CdlodNode& n, float camX, float camY, float camZ,
                     const std::vector<float>& ranges, float morphRegion, VisibleFn&& visible, EmitFn&& emit)
{
    const int level = n.level;
    const float distSq = CdlodNodeDistanceSq(n, camX, camY, camZ);
    if (distSq > ranges[level] * ranges[level]) return false;
    if (!visible(n)) return true;
    if (level == 0 || distSq > ranges[level - 1] * ranges[level - 1])
    {
        EmitCdlodNode(n, level, ranges, morphRegion, emit);
        return true;
    }
    for (int c = 0; c < 4; ++c)
    {
        CdlodNode child = n;
        child.size *= 0.5f;
        child.level = level - 1;
        child.originX += (c & 1) * child.size;
        child.originZ += (c >> 1) * child.size;
        if (!SelectFlatCdlod(child, camX, camY, camZ, ranges, morphRegion, visible, emit) && visible(child))
            EmitCdlodNode(child, child.level, ranges, morphRegion, emit);
    }
    return true;
}

// Recursive builder for BuildCdlodTree. `leafBounds(oxTexel, ozTexel, spanTexels,
// minY, maxY)` fills the world-height extent of a leaf covering the texel square
// [ox, ox+span] x [oz, oz+span]; interior nodes aggregate their children.
template <typename LeafBoundsFn>
inline int BuildCdlodNode(std::vector<CdlodNode>& tree, float grid, int leafTexels, int oxTexel, int ozTexel,
                          int spanTexels, int level, LeafBoundsFn& leafBounds)
{
    CdlodNode n{};
    n.originX = oxTexel * grid;
    n.originZ = ozTexel * grid;
    n.size = spanTexels * grid;
    n.level = level;
    n.child[0] = n.child[1] = n.child[2] = n.child[3] = -1;

    if (spanTexels <= leafTexels)
    {
        float mn = 1e30f;
        float mx = -1e30f;
        leafBounds(oxTexel, ozTexel, spanTexels, mn, mx);
        n.minY = mn;
        n.maxY = mx;
    }
    else
    {
        const int half = spanTexels / 2;
        int c[4];
        c[0] = BuildCdlodNode(tree, grid, leafTexels, oxTexel, ozTexel, half, level - 1, leafBounds);
        c[1] = BuildCdlodNode(tree, grid, leafTexels, oxTexel + half, ozTexel, half, level - 1, leafBounds);
        c[2] = BuildCdlodNode(tree, grid, leafTexels, oxTexel, ozTexel + half, half, level - 1, leafBounds);
        c[3] = BuildCdlodNode(tree, grid, leafTexels, oxTexel + half, ozTexel + half, half, level - 1, leafBounds);
        n.minY = 1e30f;
        n.maxY = -1e30f;
        for (int i = 0; i < 4; i++)
        {
            n.minY = std::min(n.minY, tree[c[i]].minY);
            n.maxY = std::max(n.maxY, tree[c[i]].maxY);
            n.child[i] = c[i];
        }
    }

    const int idx = static_cast<int>(tree.size());
    tree.push_back(n);
    return idx;
}

// Smallest power-of-two multiple of `leafTexels` that covers `coverageTexels` — the
// span a CDLOD root must have so every level halves cleanly down to a leaf.
inline int CdlodRootTexels(int coverageTexels, int leafTexels)
{
    int r = leafTexels;
    while (r < coverageTexels)
    {
        r *= 2;
    }
    return r;
}

// Top-left texel of a `rootTexels`-wide root centred over a `range`-wide map, snapped
// to a whole leaf so the in-map leaves keep the exact texel alignment of an
// un-extended (origin 0) tree — extending the tree then adds an off-map border without
// disturbing the in-map tessellation. Returns 0 when the root barely exceeds the map.
inline int CdlodCenteredOrigin(int rootTexels, int range, int leafTexels)
{
    int margin = (rootTexels - range) / 2;
    margin -= margin % leafTexels; // snap down to a whole leaf
    return -margin;
}

// Builds a CDLOD quadtree with a `rootTexels` x `rootTexels` root (must be a
// power-of-two multiple of `leafTexels`, e.g. from CdlodRootTexels) whose top-left
// corner is texel (originTexelX, originTexelZ) — usually (0,0), but water offsets it
// negative to centre a larger tree over the map so the ocean reaches past the edges.
// `grid` is the world spacing between texels; `leafBounds` supplies each leaf's
// world-height extent (see BuildCdlodNode). Fills `tree` (root last), and reports the
// root index, the tree depth, and a leaf's world size. Terrain and water share this:
// they differ only in the emit/visible functors used at selection time.
template <typename LeafBoundsFn>
inline void BuildCdlodTree(int rootTexels, int originTexelX, int originTexelZ, float grid, int leafTexels,
                           LeafBoundsFn leafBounds, std::vector<CdlodNode>& tree, int& rootIndex, int& numLevels,
                           float& leafSize)
{
    tree.clear();
    rootIndex = -1;
    numLevels = 0;
    if (rootTexels <= 0 || grid <= 0.0f || leafTexels <= 0)
    {
        return;
    }

    numLevels = 1;
    int t = leafTexels;
    while (t < rootTexels)
    {
        t *= 2;
        numLevels++;
    }
    leafSize = leafTexels * grid;
    rootIndex = BuildCdlodNode(tree, grid, leafTexels, originTexelX, originTexelZ, rootTexels, numLevels - 1,
                               leafBounds);
}

} // namespace Poseidon
