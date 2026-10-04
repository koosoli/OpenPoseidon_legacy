#pragma once
#include <array>
#include <cmath>
#include <limits>

namespace Poseidon
{
// Reject malformed endpoint coordinates before integer conversion, unlocking
// the vehicle, or allocating an operational map. Half the signed-int range
// leaves room for coordinate differences in the existing map arithmetic.
inline bool SimplePathEndpointsAdmitted(const std::array<float, 3>& from,
                                        const std::array<float, 3>& to, float inverseGrid)
{
    if (!std::isfinite(inverseGrid) || inverseGrid <= 0)
        return false;
    for (const auto& endpoint : {from, to})
    {
        for (float coordinate : endpoint)
            if (!std::isfinite(coordinate))
                return false;
        for (int axis : {0, 2})
        {
            const float grid = endpoint[axis] * inverseGrid;
            if (!std::isfinite(grid) || std::abs(grid) >= float(std::numeric_limits<int>::max() / 2))
                return false;
        }
    }
    return true;
}
}
