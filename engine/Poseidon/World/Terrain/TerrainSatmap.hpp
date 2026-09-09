#pragma once
#include <array>
#include <cmath>

namespace Poseidon
{
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
