#pragma once

#include <Poseidon/Graphics/Rendering/Draw/SpecLods.hpp>
#include <Poseidon/World/Model/LodPurpose.hpp>
#include <cmath>


namespace Poseidon
{
inline bool IsSpec(float resolution, float spec)
{
    return fabs(resolution - spec) < spec * 1e-3;
}

// AST-018: one table, in the IR. This used to spell the list out again here,
// and had drifted -- it omitted the gunner fire-geometry sentinel, so a LOD that
// is by definition never drawn was being loaded with normals as if it were.
static inline bool ResolGeometryOnly(float resolution)
{
    return Model::IsGeometryOnlyLod(Model::ClassifyLodResolution(resolution));
}

struct SortVertex
{
    int vertex, point, prior;
};

inline int CmpSortVertex(const SortVertex* v0, const SortVertex* v1)
{
    int d;
    d = v0->prior - v1->prior;
    if (d)
    {
        return d;
    }
    d = v0->point - v1->point;
    return d;
}

} // namespace Poseidon
