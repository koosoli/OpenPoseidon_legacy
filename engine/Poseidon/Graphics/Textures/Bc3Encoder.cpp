#include <Poseidon/Graphics/Textures/Bc3Encoder.hpp>
#include <algorithm>
#include <cstring>
#include <limits>
#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

namespace Poseidon
{
bool MeasureBc3EncodingFootprint(int width, int height, Bc3EncodingFootprint& out)
{
    out = {};
    if (width < 4 || height < 4) return false;
    const uint64_t rgba = static_cast<uint64_t>(width) * height * 4;
    uint64_t blockBytes = 0, largestNext = 0;
    int w = width, h = height, levels = 0;
    while (true)
    {
        blockBytes += ((static_cast<uint64_t>(w) + 3) / 4) * ((static_cast<uint64_t>(h) + 3) / 4) * 16;
        ++levels;
        if (w <= 4 && h <= 4) break;
        w = std::max(w / 2, 1); h = std::max(h / 2, 1);
        largestNext = std::max(largestNext, static_cast<uint64_t>(w) * h * 4);
    }
    // GPU byte counts and mip offsets are 32-bit. Also bounds subsequent int
    // edge arithmetic, without permitting an overflowing allocation estimate.
    if (rgba > UINT32_MAX || blockBytes > UINT32_MAX) return false;
    const uint64_t scratch = 2 * rgba + largestNext + blockBytes + levels * sizeof(uint32_t);
    if (scratch > std::numeric_limits<size_t>::max()) return false;
    out = {static_cast<size_t>(rgba), static_cast<size_t>(blockBytes), static_cast<size_t>(scratch), levels};
    return true;
}

bool EncodeBc3ChainRGBA(const uint8_t* rgba, int width, int height, std::vector<uint8_t>& blocks,
                        std::vector<uint32_t>& offsets, int& levels, const Bc3EncodingObserver* observer)
{
    Bc3EncodingFootprint footprint;
    if (!rgba || !MeasureBc3EncodingFootprint(width, height, footprint)) return false;
    std::vector<uint8_t> level(rgba, rgba + footprint.rgbaBytes);
    int w = width, h = height;
    blocks.clear(); offsets.clear(); levels = 0;
    // Avoid repeated output reallocations and make the worker reservation finite.
    blocks.reserve(footprint.blockBytes); offsets.reserve(footprint.levels);
    uint8_t block[64], out[16];
    while (true)
    {
        offsets.push_back(static_cast<uint32_t>(blocks.size()));
        const int bw = (w + 3) / 4, bh = (h + 3) / 4;
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx)
            {
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x)
                    {
                        const int sx = std::min(bx * 4 + x, w - 1), sy = std::min(by * 4 + y, h - 1);
                        std::memcpy(block + (y * 4 + x) * 4, level.data() + (static_cast<size_t>(sy) * w + sx) * 4, 4);
                    }
                stb_compress_dxt_block(out, block, 1, STB_DXT_NORMAL);
                blocks.insert(blocks.end(), out, out + 16);
            }
        ++levels;
        if (observer && levels == 1 && observer->afterTopMip)
            observer->afterTopMip(observer->context, blocks.size(), blocks.capacity());
        if (w <= 4 && h <= 4) break;
        const int nw = std::max(w / 2, 1), nh = std::max(h / 2, 1);
        std::vector<uint8_t> next(static_cast<size_t>(nw) * nh * 4);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c)
                {
                    const int x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
                    const int y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
                    const int sum = level[(static_cast<size_t>(y0) * w + x0) * 4 + c] +
                                    level[(static_cast<size_t>(y0) * w + x1) * 4 + c] +
                                    level[(static_cast<size_t>(y1) * w + x0) * 4 + c] +
                                    level[(static_cast<size_t>(y1) * w + x1) * 4 + c];
                    next[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
                }
        level.swap(next); w = nw; h = nh;
    }
    return true;
}
}
