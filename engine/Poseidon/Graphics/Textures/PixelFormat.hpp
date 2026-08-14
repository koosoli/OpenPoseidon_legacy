#pragma once

#include <cstdint>
#include <cstddef>

namespace Poseidon
{

// Canonical pixel encoding, independent of container/envelope (PAA, PAC, PNG, DDS).
enum class PixelFormat : uint8_t
{
    Unknown = 0,
    P8,       // 8-bit palette indexed
    AI88,     // Alpha + Intensity 16-bit
    RGB565,   // 16-bit RGB (5:6:5)
    ARGB1555, // 16-bit ARGB (1:5:5:5)
    ARGB4444, // 16-bit ARGB (4:4:4:4)
    ARGB8888, // 32-bit ARGB
    RGBA8888, // 32-bit RGBA (canonical working format)
    DXT1,     // BC1 — 4bpp, 1-bit alpha
    DXT3,     // BC2 — 8bpp, explicit alpha
    DXT5,     // BC3 — 8bpp, interpolated alpha
    // The DXGI-era block formats. Reachable only through a DDS/EDDS "DX10"
    // header — the Arma-era PAA container has no magic word for them, which is
    // why their paaMagic is 0 and PAA round-tripping does not offer them.
    BC4,  // 4bpp, one channel
    BC5,  // 8bpp, two channels
    BC7,  // 8bpp, RGBA
    R8,   // 8bpp uncompressed single channel — a mask, not a palette index (cf. P8)
    Count
};

struct PixelFormatInfo
{
    PixelFormat format;
    const char* name;
    const char* description;
    int bitsPerPixel;
    bool isCompressed;
    bool hasAlpha;
    uint16_t paaMagic; // PAA file magic word, 0 if N/A
};

// Static, queryable format catalog
class PixelFormatRegistry
{
  public:
    // Get info for a known format. Returns info with format==Unknown for invalid input.
    static const PixelFormatInfo& Get(PixelFormat fmt);

    // Find by name (case-insensitive). Returns nullptr if not found.
    static const PixelFormatInfo* FindByName(const char* name);

    // Find by PAA magic word. Returns nullptr if not found.
    static const PixelFormatInfo* FindByPaaMagic(uint16_t magic);

    // Number of registered formats (excluding Unknown and Count)
    static int FormatCount();

    // Iterate all registered formats
    static const PixelFormatInfo* AllFormats();
    static size_t AllFormatsCount();
};

} // namespace Poseidon
