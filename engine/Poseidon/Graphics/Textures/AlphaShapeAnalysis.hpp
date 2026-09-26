#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace Poseidon
{

// Pure source-pixel facts. No renderer policy, environment, cache or worker state.
struct AlphaShapeAnalysis
{
    double meanClearNeighbours = 4.0; // over clear texels, mean count of clear four-neighbors
    double clusteredClearFrac = 1.0; // clear texels with at least two clear neighbors
    double partialBandWidth = 0.0; // partial texels divided by clear/nonclear perimeter
    bool measured = false;
};

namespace detail
{
template<size_t FixedStride>
inline AlphaShapeAnalysis MeasureAlphaShapeRows(const uint8_t* pixels, int w, int h,
                                               size_t dynamicStride, size_t offset)
{
    AlphaShapeAnalysis shape;
    const size_t stride = FixedStride ? FixedStride : dynamicStride;
    const size_t rowBytes = static_cast<size_t>(w) * stride;
    size_t clear = 0, clustered = 0, clearAdjacency = 0, partial = 0, perimeter = 0;
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* row = pixels + static_cast<size_t>(y) * rowBytes + offset;
        const bool hasUp = y > 0, hasDown = y + 1 < h;
        const uint8_t* up = hasUp ? row - rowBytes : nullptr;
        const uint8_t* down = hasDown ? row + rowBytes : nullptr;
        for (int x = 0; x < w; ++x)
        {
            const size_t column = static_cast<size_t>(x) * stride;
            const uint8_t* sample = row + column;
            const int a = *sample;
            // Opaque samples do not update any counter. Keep this fast exit
            // before the partial/clear branch, including for sparse masks.
            if (a == 255) continue;
            if (a != 0)
            {
                ++partial;
                continue;
            }
            ++clear;
            const bool hasLeft = x > 0, hasRight = x + 1 < w;
            int clearNeighbours = 0;
            if (hasLeft) clearNeighbours += *(sample - stride) == 0;
            if (hasRight) clearNeighbours += sample[stride] == 0;
            if (hasUp) clearNeighbours += up[column] == 0;
            if (hasDown) clearNeighbours += down[column] == 0;
            // Image-edge neighbors are omitted, never invented as solid or clear.
            const int validNeighbours = int(hasLeft) + int(hasRight) + int(hasUp) + int(hasDown);
            clearAdjacency += static_cast<size_t>(clearNeighbours);
            if (clearNeighbours >= 2) ++clustered;
            perimeter += static_cast<size_t>(validNeighbours - clearNeighbours);
        }
    }
    shape.meanClearNeighbours = clear ? static_cast<double>(clearAdjacency) / static_cast<double>(clear) : 4.0;
    shape.clusteredClearFrac = clear ? static_cast<double>(clustered) / static_cast<double>(clear) : 1.0;
    shape.partialBandWidth = perimeter ? static_cast<double>(partial) / static_cast<double>(perimeter)
                                       : (partial ? std::numeric_limits<double>::infinity() : 0.0);
    shape.measured = true;
    return shape;
}
} // namespace detail

// Caller supplies the same readable source image/stride/offset as the original
// renderer scan. No image allocation, subsampling or threshold decisions occur.
inline AlphaShapeAnalysis MeasureAlphaShapeAnalysis(const uint8_t* pixels, int w, int h,
                                                   size_t stride, size_t offset)
{
    if (!pixels || w <= 0 || h <= 0) return {};
    if (stride == 1) return detail::MeasureAlphaShapeRows<1>(pixels, w, h, stride, offset);
    if (stride == 4) return detail::MeasureAlphaShapeRows<4>(pixels, w, h, stride, offset);
    return detail::MeasureAlphaShapeRows<0>(pixels, w, h, stride, offset);
}

} // namespace Poseidon
