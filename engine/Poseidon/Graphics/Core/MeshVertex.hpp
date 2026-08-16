#pragma once

#include <Poseidon/Foundation/Math/Math3DP.hpp>
#include <Poseidon/Graphics/Rendering/Primitives/Vertex.hpp>

namespace Poseidon
{

struct SVertex
{
    Vector3P pos;
    // Normals are negated (matches the D3D convention).
    Vector3P norm;
    UVPair t0;
    // Per-vertex terrain-conform selector for GPU vegetation conforming, from the
    // shape's ClipLand hints: 0 = rigid, 1 = ClipLandKeep, 2 = ClipLandOn. Read by the
    // mesh vertex shader (with a per-instance conform mode) to conform to SurfaceY.
    uint32_t conform;
    // Optional authored tangent/binormal, carried by ODOL into the production
    // WGPU mesh. Zero means this source has no decoded tangent frame.
    Vector3P tangent;
    Vector3P binormal;
    // Second UV set (`tex1`), or a copy of t0 when the shape carries none. Real
    // Virtuality's Multi materials address their blend mask / macro / ambient-shadow
    // stages through it. Appended, so every earlier offset is unchanged.
    UVPair t1;
};

} // namespace Poseidon
