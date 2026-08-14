#pragma once

#include <Poseidon/Graphics/Textures/DDSConverter.hpp>
#include <Poseidon/Graphics/Textures/PixelFormat.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Poseidon
{

// Enfusion .edds reader (DZ-002).
//
// DayZ's textures are ".edds", and the name is misleading: the file opens with a
// standard 128-byte DDS header, but the pixel payload that would follow it in a real
// DDS is replaced by a chunked, optionally LZ4-compressed one. Handing an .edds to
// DDSConverter::ReadDDSBuffer therefore does not fail loudly -- it reads the header,
// takes the first bytes of the chunk table as mip 0, and produces garbage.
//
// The layout, fitted against the owner's install and closing to the byte on all 454
// .edds files in it:
//
//   [128-byte DDS header]            dwReserved1[1] == 'ENF1' marks the variant
//   [20-byte DXT10 header]           only when ddspf.dwFourCC == 'DX10'
//   [mipCount x {FourCC tag, u32}]   chunk table, SMALLEST mip first
//   [chunk payloads, in table order]
//
// 'COPY' stores the mip raw and its chunk size always equals the size the mip's
// dimensions and block format require (1,846 of 1,846 chunks). 'LZ4 ' stores it as
// linked 64 KiB sub-blocks -- see Foundation::Lz4Block for why the linking matters.
//
// Mip order is the interesting part, because it is backwards from DDS: the table
// runs smallest to largest. Read the other way round every texture in the game is
// a 1x1 smear, which is a bug that renders rather than one that errors.
struct EddsImage
{
    PixelFormat format = PixelFormat::Unknown;
    int width = 0;
    int height = 0;
    // [0] is the largest level, matching DDSFile and the rest of the engine --
    // the on-disk order is reversed during the read.
    std::vector<DDSMipLevel> mipmaps;
    bool hasAlpha = false;
    // Empty on success. Names the exact constraint that failed, because this is the
    // diagnostic entry point for a whole generation of assets and a bare "failed to
    // load" is what made the ODOL work a hex editor exercise (DZ-001).
    std::string error;

    bool valid() const { return width > 0 && height > 0 && !mipmaps.empty() && format != PixelFormat::Unknown; }
};

// True when the buffer is a DDS carrying Enfusion's 'ENF1' marker. A plain DDS
// returns false and should go to DDSConverter::ReadDDSBuffer instead.
bool IsEddsBuffer(const void* data, size_t size);

// Decodes the container. On failure the returned image is invalid and `error` says
// why; it never throws.
EddsImage ReadEddsBuffer(const void* data, size_t size);

} // namespace Poseidon
