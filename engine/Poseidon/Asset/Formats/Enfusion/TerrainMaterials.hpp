// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// RFG-006 -- the `.ttile` TMAT decoder, moved out of the asset CLI into the engine
// library so BOTH the converting exporter and the native loader share one copy.
//
// It lived in `apps/tools/Tools/commands/PakCommand.cpp` because the tool was the
// only thing that had ever needed it. RFG-005 gave the engine a native Enfusion
// terrain path, and that path draws white precisely because this decoder was on the
// wrong side of the line: heights loaded, surfaces did not. Nothing below changed in
// the move except the namespace it sits in.

#include <Poseidon/Asset/Formats/Enfusion/EnfusionIff.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Poseidon::Asset::Formats::Enfusion
{

//! `.ttile`'s TMAT chunk: which authored surface material covers each square metre.
//!
//! TMAT holds one nested `BMAT` chunk per 64 m sub-cell -- `u16 subX, u16 subY,
//! u16 count, count x u16 matIdx` (indices into the `.terr` MATS list), then a
//! blend payload that is empty exactly when count == 1. The payload is a
//! quadtree over a 64x64 mask of the sub-cell, one texel per metre.
//!
//! The palette is the trap. Every TMAT palette is sorted ASCENDING by material
//! index, so entry 0 is the lowest index present and NOT the base layer. Reading
//! entry 0 as "the material here" renders Everon as 41% dirt.
namespace Tmat
{

//! Bits per palette entry in a packed sample block: ceil(log2(count)), min 1.
inline int BitsFor(size_t count)
{
    int bits = 1;
    while ((static_cast<size_t>(1) << bits) < count)
        ++bits;
    return bits;
}

//! Little-endian bit order within each byte, `width` bits per sample.
inline uint8_t SampleAt(const uint8_t* data, size_t index, int width)
{
    uint32_t value = 0;
    for (int k = 0; k < width; ++k)
    {
        const size_t bit = index * static_cast<size_t>(width) + static_cast<size_t>(k);
        value |= static_cast<uint32_t>((data[bit >> 3] >> (bit & 7)) & 1u) << k;
    }
    return static_cast<uint8_t>(value);
}

//! One sub-cell's records, pointing into the caller's TMAT payload.
struct SubCell
{
    uint16_t subX = 0;
    uint16_t subY = 0;
    std::vector<uint16_t> palette;
    const uint8_t* blend = nullptr;
    size_t blendSize = 0;
};

//! Walk TMAT's nested BMAT chunks. BMAT sizes are big-endian like every other
//! Enfusion chunk header; the records themselves are little-endian.
inline bool ReadSubCells(const uint8_t* data, size_t size, std::vector<SubCell>& out, std::string& error)
{
    size_t at = 0;
    while (at + 8 <= size)
    {
        if (ReadBe32(data + at) != FourCC("BMAT").value)
        {
            error = "TMAT: expected BMAT";
            return false;
        }
        const size_t chunk = ReadBe32(data + at + 4);
        if (at + 8 + chunk > size || chunk < 6)
        {
            error = "TMAT: BMAT overruns";
            return false;
        }
        const uint8_t* p = data + at + 8;
        SubCell cell;
        cell.subX = ReadLe16(p, 0);
        cell.subY = ReadLe16(p, 2);
        const size_t count = ReadLe16(p, 4);
        if (count == 0 || 6 + 2 * count > chunk)
        {
            error = "TMAT: BMAT palette overruns";
            return false;
        }
        cell.palette.resize(count);
        for (size_t i = 0; i < count; ++i)
            cell.palette[i] = ReadLe16(p, 6 + 2 * i);
        cell.blend = p + 6 + 2 * count;
        cell.blendSize = chunk - (6 + 2 * count);
        out.push_back(std::move(cell));
        at += 8 + chunk;
    }
    if (at != size)
    {
        error = "TMAT: chunk walk did not close";
        return false;
    }
    return true;
}

constexpr int kMaskEdge = 64;

//! One packed run of per-texel palette entries, pointing into the payload.
struct MaskBlock
{
    size_t texels = 0;
    const uint8_t* bits = nullptr;
};

//! Depth-first walk of the blend quadtree, in the order the mask blocks are stored.
//!
//! Pre-order and strictly quadrant-ordered: a masked leaf consumes the next block
//! the moment it is reached, and an internal child is descended into immediately,
//! so its whole subtree consumes its blocks before the parent moves on to the next
//! quadrant. Any other order -- deferring the children to a work stack, say --
//! still visits every node and still fills every texel, and pairs the wrong block
//! with the wrong leaf. It shows up as `mask block does not match its leaf` on the
//! 3% of sub-cells whose tree is deep enough for the orders to diverge, and
//! anywhere the block sizes happen to agree it would silently place the wrong
//! materials instead. Depth is bounded by the 64x64 mask, so six levels.
struct Walk
{
    const uint8_t* codes = nullptr;
    uint16_t nodes = 0;
    size_t count = 0;
    int width = 1;
    const std::vector<MaskBlock>* masks = nullptr;
    uint8_t* out = nullptr;
    std::vector<bool> seen;
    size_t used = 0;
    size_t visited = 0;
    std::string error;

    bool Node(uint16_t node, int y, int x, int size)
    {
        static constexpr int kQuad[4][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
        if (node >= nodes || seen[node])
        {
            error = "blend node revisited or out of range";
            return false;
        }
        seen[node] = true;
        ++visited;
        const int half = size / 2;
        if (half < 1)
        {
            error = "blend internal node below 1x1";
            return false;
        }
        for (int q = 0; q < 4; ++q)
        {
            const uint16_t code = ReadLe16(codes, static_cast<size_t>(node) * 8 + static_cast<size_t>(q) * 2);
            const int yy = y + kQuad[q][0] * half;
            const int xx = x + kQuad[q][1] * half;
            if ((code & 0x8000) != 0)
            {
                const uint16_t v = code & 0x3FFF;
                if (v >= count)
                {
                    error = "uniform leaf out of palette";
                    return false;
                }
                for (int dy = 0; dy < half; ++dy)
                    std::fill_n(out + static_cast<size_t>(yy + dy) * kMaskEdge + xx, half, static_cast<uint8_t>(v));
            }
            else if ((code & 0x4000) != 0)
            {
                if (used >= masks->size())
                {
                    error = "ran out of blend mask blocks";
                    return false;
                }
                const MaskBlock& block = (*masks)[used++];
                if (block.texels != static_cast<size_t>(half) * half)
                {
                    error = "blend mask block does not match its leaf";
                    return false;
                }
                for (int dy = 0; dy < half; ++dy)
                    for (int dx = 0; dx < half; ++dx)
                    {
                        const uint8_t v = SampleAt(block.bits, static_cast<size_t>(dy) * half + dx, width);
                        if (v >= count)
                        {
                            error = "blend mask sample out of palette";
                            return false;
                        }
                        out[static_cast<size_t>(yy + dy) * kMaskEdge + xx + dx] = v;
                    }
            }
            else if (!Node(code, yy, xx, half))
            {
                return false;
            }
        }
        return true;
    }
};

//! Decode one sub-cell's blend payload into 64x64 PALETTE ENTRY indices.
//!
//! `flags == 0x4000` is a raw block of 4096 packed samples. `flags == 0` is a
//! quadtree: each node holds four child codes in (top-left, top-right,
//! bottom-left, bottom-right) order, where `0x8000|v` is a uniform leaf,
//! `0x4000|v` a leaf that consumes the next packed mask block, and anything else
//! an internal node index. Mask blocks appear in DFS order and each block's
//! declared texel count is exactly the leaf's area, which is what pins the
//! traversal down.
inline bool DecodeBlend(const uint8_t* data, size_t size, size_t count, uint8_t* out, std::string& error)
{
    if (size < 4)
    {
        error = "blend payload shorter than its header";
        return false;
    }
    const uint16_t nodes = ReadLe16(data, 0);
    const uint16_t flags = ReadLe16(data, 2);
    const int width = BitsFor(count);

    if (flags == 0x4000)
    {
        if (size < 10 || ReadLe16(data, 4) != 1 || ReadLe32(data, 6) != 4096)
        {
            error = "raw blend header";
            return false;
        }
        const size_t need = static_cast<size_t>(4096) * static_cast<size_t>(width) / 8;
        if (10 + need != size)
        {
            error = "raw blend size";
            return false;
        }
        for (size_t i = 0; i < 4096; ++i)
        {
            const uint8_t v = SampleAt(data + 10, i, width);
            if (v >= count)
            {
                error = "raw blend sample out of palette";
                return false;
            }
            out[i] = v;
        }
        return true;
    }
    if (flags != 0)
    {
        error = "unknown blend flags";
        return false;
    }

    const size_t codesBytes = static_cast<size_t>(nodes) * 8;
    if (4 + codesBytes > size)
    {
        error = "blend node table overruns";
        return false;
    }
    const uint8_t* codes = data + 4;
    size_t at = 4 + codesBytes;

    // The mask-block count is omitted entirely when there are no masked leaves.
    size_t maskCount = 0;
    if (at != size)
    {
        if (at + 2 > size)
        {
            error = "blend mask count overruns";
            return false;
        }
        maskCount = ReadLe16(data, at);
        at += 2;
    }
    std::vector<MaskBlock> masks;
    masks.reserve(maskCount);
    for (size_t i = 0; i < maskCount; ++i)
    {
        if (at + 4 > size)
        {
            error = "blend mask header overruns";
            return false;
        }
        const size_t texels = ReadLe32(data, at);
        at += 4;
        const size_t bytes = texels * static_cast<size_t>(width) / 8;
        if ((texels % 16) != 0 || at + bytes > size)
        {
            error = "blend mask block overruns";
            return false;
        }
        masks.push_back({texels, data + at});
        at += bytes;
    }
    if (at != size)
    {
        error = "blend payload did not close";
        return false;
    }

    Walk walk;
    walk.codes = codes;
    walk.nodes = nodes;
    walk.count = count;
    walk.width = width;
    walk.masks = &masks;
    walk.out = out;
    walk.seen.assign(nodes, false);
    if (!walk.Node(0, 0, 0, kMaskEdge))
    {
        error = walk.error;
        return false;
    }
    // Both closures are required. Reaching every texel is not the same as
    // consuming every block: a traversal in the wrong order can fill the mask
    // completely and still have paired blocks with the wrong leaves.
    if (walk.used != masks.size() || walk.visited != nodes)
    {
        error = "blend payload left nodes or mask blocks unused";
        return false;
    }
    return true;
}

} // namespace Tmat

} // namespace Poseidon::Asset::Formats::Enfusion
