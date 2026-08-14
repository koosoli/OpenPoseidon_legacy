#include <Poseidon/Graphics/Textures/PixelFormat.hpp>
#include <cstring>
#include <cctype>

namespace Poseidon
{

// clang-format off
static constexpr PixelFormatInfo kFormats[] = {
    {PixelFormat::P8,       "P8",       "8-bit palette indexed",        8,  false, false, 0x0000},
    {PixelFormat::AI88,     "AI88",     "Alpha + Intensity 16-bit",     16, false, true,  0x8080},
    {PixelFormat::RGB565,   "RGB565",   "16-bit RGB (5:6:5)",           16, false, false, 0x0000},
    {PixelFormat::ARGB1555, "ARGB1555", "16-bit ARGB (1:5:5:5)",       16, false, true,  0x1555},
    {PixelFormat::ARGB4444, "ARGB4444", "16-bit ARGB (4:4:4:4)",       16, false, true,  0x4444},
    {PixelFormat::ARGB8888, "ARGB8888", "32-bit ARGB",                              32, false, true,  0x8888},
    {PixelFormat::RGBA8888, "RGBA8888", "32-bit RGBA (internal processing format)", 32, false, true,  0x0000},
    {PixelFormat::DXT1,     "DXT1",     "BC1 - RGB 5:6:5, 1-bit alpha", 4,  true,  true,  0xFF01},
    {PixelFormat::DXT3,     "DXT3",     "BC2 - RGB 5:6:5, explicit 4-bit alpha", 8,  true,  true,  0xFF03},
    {PixelFormat::DXT5,     "DXT5",     "BC3 - RGB 5:6:5, interpolated 8-bit alpha", 8, true, true,  0xFF05},
    {PixelFormat::BC4,      "BC4",      "BC4 - single channel, interpolated",       4,  true,  false, 0x0000},
    {PixelFormat::BC5,      "BC5",      "BC5 - two channels, interpolated",         8,  true,  false, 0x0000},
    {PixelFormat::BC7,      "BC7",      "BC7 - RGBA, mode-switched partitions",     8,  true,  true,  0x0000},
    {PixelFormat::R8,       "R8",       "8-bit single channel (mask)",              8,  false, false, 0x0000},
};
// clang-format on

static constexpr int kFormatCount = static_cast<int>(sizeof(kFormats) / sizeof(kFormats[0]));

static const PixelFormatInfo kUnknown = {PixelFormat::Unknown, "Unknown", "Unknown format", 0, false, false, 0};

static bool iequals(const char* a, const char* b)
{
    for (; *a && *b; ++a, ++b)
    {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
            return false;
    }
    return *a == *b;
}

const PixelFormatInfo& PixelFormatRegistry::Get(PixelFormat fmt)
{
    for (int i = 0; i < kFormatCount; ++i)
    {
        if (kFormats[i].format == fmt)
            return kFormats[i];
    }
    return kUnknown;
}

const PixelFormatInfo* PixelFormatRegistry::FindByName(const char* name)
{
    if (!name)
        return nullptr;
    for (int i = 0; i < kFormatCount; ++i)
    {
        if (iequals(kFormats[i].name, name))
            return &kFormats[i];
    }
    return nullptr;
}

const PixelFormatInfo* PixelFormatRegistry::FindByPaaMagic(uint16_t magic)
{
    if (magic == 0)
        return nullptr;
    for (int i = 0; i < kFormatCount; ++i)
    {
        if (kFormats[i].paaMagic == magic)
            return &kFormats[i];
    }
    return nullptr;
}

int PixelFormatRegistry::FormatCount()
{
    return kFormatCount;
}

const PixelFormatInfo* PixelFormatRegistry::AllFormats()
{
    return kFormats;
}

size_t PixelFormatRegistry::AllFormatsCount()
{
    return kFormatCount;
}

} // namespace Poseidon
