#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Poseidon
{
struct Bc3MipChain
{
    int width = 0, height = 0, levels = 0;
    std::vector<uint8_t> blocks;
    std::vector<uint32_t> offsets;
    size_t RetainedBytes() const { return blocks.capacity() + offsets.capacity() * sizeof(uint32_t); }
};
struct Bc3EncodingFootprint
{
    size_t rgbaBytes = 0, blockBytes = 0, scratchBytes = 0;
    int levels = 0;
};
// Explicit CPU observation seam, never a global hook. Called once after real
// top-mip compression, outside block/pixel loops. Scalars describe CPU output,
// not GPU acceptance/completion. No encoder buffer is borrowed by the callback.
// Context must outlive the synchronous call; exceptions propagate to its caller.
struct Bc3EncodingObserver
{
    void (*afterTopMip)(void* context, size_t completedBytes, size_t blockCapacity) = nullptr;
    void* context = nullptr;
};
// Includes caller's top RGBA copy, encoder's copy, largest downsample, output
// blocks and offsets; excludes caller's retained source and allocator overhead.
bool MeasureBc3EncodingFootprint(int width, int height, Bc3EncodingFootprint& out);
// The original wgpu CPU encoder: NORMAL stb_dxt, clamped edge blocks, rounded
// 2x2 box mips, ending when both dimensions are <=4. No globals/I/O/renderer.
bool EncodeBc3ChainRGBA(const uint8_t* rgba, int width, int height,
                        std::vector<uint8_t>& blocks, std::vector<uint32_t>& offsets, int& levels,
                        const Bc3EncodingObserver* observer = nullptr);
}
