#pragma once

#include <algorithm>

namespace Poseidon
{
// A float heightfield at this limit is 256 MiB, within Jimbo's single-allocation
// ceiling and wgpu's requested 2D texture limit. This caps refinement, not metres.
constexpr int MaxRefinedTerrainRange = 8192;

inline int BoundedTerrainSubdivision(int landRange, int requestedLog)
{
    if (landRange <= 0)
        return 0;
    int maxLog = 0;
    while (landRange <= MaxRefinedTerrainRange / 2)
    {
        landRange *= 2;
        ++maxLog;
    }
    return std::clamp(requestedLog, 0, maxLog);
}
}
