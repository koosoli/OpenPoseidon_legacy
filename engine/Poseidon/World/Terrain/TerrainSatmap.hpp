#pragma once
#include <array>
#include <cmath>
#include <algorithm>

namespace Poseidon
{
// Screen-space barycentrics shared by the CPU top-view baker and its tests.
inline bool SatmapTriangleWeights(const std::array<float,2>& a, const std::array<float,2>& b,
                                 const std::array<float,2>& c, float x, float y,
                                 std::array<float,3>& w)
{
    const float d = (b[1]-c[1])*(a[0]-c[0]) + (c[0]-b[0])*(a[1]-c[1]);
    if (!std::isfinite(d) || std::abs(d) < 1e-8f) return false;
    w[0] = ((b[1]-c[1])*(x-c[0]) + (c[0]-b[0])*(y-c[1]))/d;
    w[1] = ((c[1]-a[1])*(x-c[0]) + (a[0]-c[0])*(y-c[1]))/d;
    w[2] = 1-w[0]-w[1];
    return w[0] >= -1e-5f && w[1] >= -1e-5f && w[2] >= -1e-5f;
}
// Reserve the fallback and generated-image slots in the 4096-layer renderer.
// Large classic composites get a bounded 21.3 MiB mip chain, not a map-sized image.
inline int TerrainSatmapResolution(int textureCount, int landRange)
{
    if (textureCount <= 0 || textureCount > 4094 || landRange < 2 || landRange > 2048)
        return 0;
    return landRange <= 512 ? 1024 : 2048;
}

// World-anchored wide cell blend for the far albedo bake. The callback retains
// the source transition tile's own edge policy; this adds no baked lighting.
template<class Sample>
std::array<float,3> SampleTerrainSatmap(float x, float z, Sample sample)
{
    const int bx = static_cast<int>(std::floor(x-0.5f));
    const int bz = static_cast<int>(std::floor(z-0.5f));
    const float fx = x-0.5f-bx, fz = z-0.5f-bz;
    const float sx = fx*fx*(3-2*fx), sz = fz*fz*(3-2*fz);
    std::array<float,3> result{};
    for (int j=0; j<2; ++j) for (int i=0; i<2; ++i)
    {
        const auto color = sample(bx+i,bz+j,x,z);
        const float weight = (i ? sx : 1-sx)*(j ? sz : 1-sz);
        for (int c=0; c<3; ++c) result[c] += color[c]*weight;
    }
    return result;
}
}
