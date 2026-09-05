#pragma once

#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>

// Per-mip pitch / size math for texture uploads.
//
// `glCompressedTexSubImage2D` and friends take the tight-packed size of the
// *target mip*, not the base mip — passing the base size for a sub-upload
// corrupts adjacent mips.  For uncompressed formats the same bug class shows
// up as a row-pitch mismatch: the destination's row stride may differ from the
// source's, and using the source pitch to walk destination rows reads / writes
// wrong memory.
//
// `MipmapLayout` answers both questions in one place:
//   tightPitch  — bytes from row N to row N+1 in tight-packed source data (for
//                 block formats this is one block-row of 4 source rows).
//   rowCount    — number of (block-)rows in the mip.
//   dataSize    — tightPitch * rowCount, the byte count any sub-upload must
//                 pass to glCompressedTexSubImage2D.
//
// Pure free function; no GL, no engine state.  Sharing it keeps the
// per-mip (not base) invariant identical across every upload path.

namespace Poseidon
{
namespace render::mipmap
{

struct Layout
{
    int tightPitch; // bytes per source row (block-row for DXT)
    int rowCount;   // number of source rows (block-rows for DXT)
    int dataSize;   // tightPitch * rowCount
};

constexpr Layout ComputeLayout(PacFormat format, int w, int h) noexcept
{
    Layout out{};

    switch (format)
    {
        case PacDXT1:
        case PacBC4:
        {
            // 4x4 blocks, 8 bytes per block.  BC4 is one channel at the same block
            // size as BC1 (RFG-047).
            int blockCols = (w + 3) / 4;
            int blockRows = (h + 3) / 4;
            out.tightPitch = blockCols * 8;
            out.rowCount = blockRows;
            if (out.tightPitch < 8)
                out.tightPitch = 8;
            if (out.rowCount < 1)
                out.rowCount = 1;
            break;
        }
        case PacDXT2:
        case PacDXT3:
        case PacDXT4:
        case PacDXT5:
        case PacBC5:
        case PacBC7:
        {
            // 4x4 blocks, 16 bytes per block.  BC5 (two channels) and BC7 (RGBA) share
            // BC2/BC3's block size (RFG-047).
            int blockCols = (w + 3) / 4;
            int blockRows = (h + 3) / 4;
            out.tightPitch = blockCols * 16;
            out.rowCount = blockRows;
            if (out.tightPitch < 16)
                out.tightPitch = 16;
            if (out.rowCount < 1)
                out.rowCount = 1;
            break;
        }
        case PacARGB8888:
            out.tightPitch = w * 4;
            out.rowCount = h;
            break;
        default:
            // 16-bit linear formats (ARGB1555, RGB565, ARGB4444, AI88…)
            out.tightPitch = w * 2;
            out.rowCount = h;
            break;
    }

    out.dataSize = out.tightPitch * out.rowCount;
    return out;
}

} // namespace render::mipmap

} // namespace Poseidon
