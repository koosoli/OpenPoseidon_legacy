#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace Poseidon::Streaming
{

enum class DemandStatus
{
    Complete,
    Invalid,
    CapacityExceeded
};

struct SimulationDemand
{
    DemandStatus status = DemandStatus::Invalid;
    std::vector<uint32_t> cells;
};

// Pure planning only: does not load objects, pin identities, or establish collision clearance.
// padding includes query radius AND a proven upper bound on placement extent. The existing
// modern index stores placement origins, so an unproven 25 m padding is insufficient for
// large modern buildings. Admission must separately establish that every returned cell is ready.
// Refused plans contain no usable prefix; callers must propagate unknown, never "clear".
inline SimulationDemand PlanSimulationDemand(double fromX, double fromZ, double toX, double toZ,
                                            double padding, double cellSize, uint32_t side,
                                            size_t maxCells)
{
    SimulationDemand out;
    if (!std::isfinite(fromX) || !std::isfinite(fromZ) || !std::isfinite(toX) || !std::isfinite(toZ) ||
        !std::isfinite(padding) || padding < 0 || !std::isfinite(cellSize) || cellSize <= 0 ||
        side == 0 || side > 65535 || maxCells > 65536)
        return out;
    const double extent = side * cellSize;
    const double dx = toX - fromX, dz = toZ - fromZ;
    if (!std::isfinite(extent + padding) || !std::isfinite(dx) || !std::isfinite(dz))
        return out;
    // Slab arithmetic can round a corner to opposite sides when endpoints are reversed.
    // Expand conservatively by a scale-aware roundoff allowance, never remove a touched cell.
    const double roundoff = 8 * std::numeric_limits<double>::epsilon() *
                            std::max({std::abs(fromX), std::abs(fromZ), std::abs(toX), std::abs(toZ),
                                      extent, padding, 1.0});

    auto clip = [](double origin, double delta, double lo, double hi, double& begin, double& end)
    {
        if (delta == 0)
            return origin >= lo && origin <= hi;
        double a = (lo - origin) / delta, b = (hi - origin) / delta;
        if (a > b)
            std::swap(a, b);
        begin = std::max(begin, a);
        end = std::min(end, b);
        return begin <= end;
    };
    double begin = 0, end = 1;
    out.status = DemandStatus::Complete;
    if (!clip(fromX, dx, -padding, extent + padding, begin, end) ||
        !clip(fromZ, dz, -padding, extent + padding, begin, end))
        return out;

    auto firstCell = [cellSize, side, roundoff](double position)
    {
        position -= roundoff;
        return static_cast<uint32_t>(std::clamp(std::floor(position / cellSize), 0.0, double(side - 1)));
    };
    auto lastCell = [cellSize, side, roundoff](double position)
    {
        return static_cast<uint32_t>(std::clamp(std::floor((position + roundoff) / cellSize), 0.0, double(side - 1)));
    };
    const double z0 = fromZ + begin * dz, z1 = fromZ + end * dz;
    const uint32_t firstZ = firstCell(std::min(z0, z1) - padding);
    const uint32_t lastZ = lastCell(std::max(z0, z1) + padding);
    // Each crossed row contributes at least one cell. Reject before a potentially huge walk.
    if (size_t(lastZ - firstZ) + 1 > maxCells)
    {
        out.status = DemandStatus::CapacityExceeded;
        return out;
    }
    for (uint32_t z = firstZ; z <= lastZ; ++z)
    {
        double rowBegin = begin, rowEnd = end;
        if (!clip(fromZ, dz, z * cellSize - padding - roundoff,
                  (z + 1) * cellSize + padding + roundoff, rowBegin, rowEnd))
            continue;
        const double x0 = fromX + rowBegin * dx, x1 = fromX + rowEnd * dx;
        const uint32_t firstX = firstCell(std::min(x0, x1) - padding);
        const uint32_t lastX = lastCell(std::max(x0, x1) + padding);
        const size_t count = size_t(lastX - firstX) + 1;
        if (count > maxCells - out.cells.size())
        {
            out.status = DemandStatus::CapacityExceeded;
            out.cells.clear();
            return out;
        }
        for (uint32_t x = firstX; x <= lastX; ++x)
            out.cells.push_back(z * side + x);
    }
    return out;
}

} // namespace Poseidon::Streaming
