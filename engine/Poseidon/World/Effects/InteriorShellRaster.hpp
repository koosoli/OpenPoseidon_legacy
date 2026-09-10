#pragma once

#include <Poseidon/Foundation/Math/Math3D.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace Poseidon
{
// Visit only cells whose seven legacy samples can reach this component.
// One extra cell makes the broad phase conservative at floating-point edges.
template <class Contains>
void RasterizeInteriorShellComponent(std::vector<unsigned char>& labels, const int (&dim)[3], Vector3Par gridMin,
                                     float cell, Vector3Par mins, Vector3Par maxs, Contains contains)
{
    int first[3], last[3];
    for (int a = 0; a < 3; ++a)
    {
        first[a] = 0;
        last[a] = dim[a] - 1;
        const float lo = (mins[a] - gridMin[a]) / cell, hi = (maxs[a] - gridMin[a]) / cell;
        if (std::isfinite(lo) && std::isfinite(hi))
        {
            first[a] = static_cast<int>(std::clamp(std::floor(lo) - 1.0f, 0.0f, float(dim[a])));
            last[a] = static_cast<int>(std::clamp(std::ceil(hi) + 1.0f, -1.0f, float(dim[a] - 1)));
        }
    }
    const float half = cell * 0.5f;
    for (int x = first[0]; x <= last[0]; ++x)
        for (int y = first[1]; y <= last[1]; ++y)
            for (int z = first[2]; z <= last[2]; ++z)
            {
                auto& label = labels[(x * dim[1] + y) * dim[2] + z];
                if (label == 0xFE)
                    continue;
                const Vector3 p = gridMin + Vector3((x + 0.5f) * cell, (y + 0.5f) * cell, (z + 0.5f) * cell);
                const Vector3 samples[7] = {p,
                                            p + Vector3(half, 0, 0),
                                            p - Vector3(half, 0, 0),
                                            p + Vector3(0, half, 0),
                                            p - Vector3(0, half, 0),
                                            p + Vector3(0, 0, half),
                                            p - Vector3(0, 0, half)};
                for (const Vector3& s : samples)
                {
                    if (s.X() < mins.X() || s.X() > maxs.X() || s.Y() < mins.Y() || s.Y() > maxs.Y() ||
                        s.Z() < mins.Z() || s.Z() > maxs.Z())
                        continue;
                    if (contains(s))
                    {
                        label = 0xFE;
                        break;
                    }
                }
            }
}
} // namespace Poseidon
