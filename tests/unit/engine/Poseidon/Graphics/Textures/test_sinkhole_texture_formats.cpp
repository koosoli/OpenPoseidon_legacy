// test_sinkhole_texture_formats.cpp - SINKHOLE W0: native PNG and plain-DDS layouts.
//
// PNG: the stb source reports ARGB8888 (the format both renderers upload from a decoded
// source) and classifies the alpha it decoded instead of discarding it.
//
// DDS: plain (non-Enfusion) .dds files go through DDSConverter::ReadDDSBuffer. The
// uncompressed layouts are read through their bit masks, DXT2/DXT4 are accepted, and a
// DXT1 that uses its punch-through colour is reported as transparent. These inputs are
// built byte by byte from the format.

#include <catch2/catch_test_macros.hpp>

#include "test_fixtures.hpp"

#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <Poseidon/Graphics/Textures/DDSConverter.hpp>
#include <Poseidon/Graphics/Textures/DdsImport.hpp>
#include <Poseidon/Graphics/Textures/JpgImport.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

namespace DDSConverter = Poseidon::DDSConverter;
using Poseidon::DDSFile;
using Poseidon::GTextureSourceJPEGFactory;
using Poseidon::PacARGB8888;
using Poseidon::PacDXT1;
using Poseidon::PacLevelMem;
using Poseidon::PixelFormat;
using Poseidon::SelectTextureSourceFactory;
using Poseidon::TextureSourceDDS;
using Poseidon::TextureSourceJPEG;

namespace
{

constexpr int kMaxMips = 32;

constexpr uint32_t kFourCC = 0x4;
constexpr uint32_t kRgb = 0x40;
constexpr uint32_t kAlphaPixels = 0x1;
constexpr uint32_t kAlpha = 0x2;
constexpr uint32_t kLuminance = 0x20000;

void PutU32(std::vector<uint8_t>& b, size_t at, uint32_t v)
{
    std::memcpy(b.data() + at, &v, 4);
}

uint32_t FourCC(const char* s)
{
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}

// A plain 128-byte DDS header (no Enfusion marker), one mip level.
std::vector<uint8_t> PlainHeader(int w, int h, uint32_t pfFlags, uint32_t fourCC, uint32_t bits, uint32_t r,
                                 uint32_t g, uint32_t b, uint32_t a)
{
    std::vector<uint8_t> f(128, 0);
    PutU32(f, 0, 0x20534444); // "DDS "
    PutU32(f, 4, 124);
    PutU32(f, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000); // caps|height|width|pixelformat|mipmapcount
    PutU32(f, 12, uint32_t(h));
    PutU32(f, 16, uint32_t(w));
    PutU32(f, 28, 1);
    PutU32(f, 76, 32);
    PutU32(f, 80, pfFlags);
    PutU32(f, 84, fourCC);
    PutU32(f, 88, bits);
    PutU32(f, 92, r);
    PutU32(f, 96, g);
    PutU32(f, 100, b);
    PutU32(f, 104, a);
    return f;
}

template <typename T> void Append(std::vector<uint8_t>& f, std::initializer_list<T> values)
{
    for (T v : values)
    {
        const size_t at = f.size();
        f.resize(at + sizeof(T));
        std::memcpy(f.data() + at, &v, sizeof(T));
    }
}

} // namespace

// ---------------------------------------------------------------- PNG

TEST_CASE("PNG: dispatcher routes .png and .tga to the image source", "[Graphics][PNG][sinkhole]")
{
    REQUIRE(SelectTextureSourceFactory("forest\\leaf.png") == GTextureSourceJPEGFactory);
    REQUIRE(SelectTextureSourceFactory("forest\\leaf.PNG") == GTextureSourceJPEGFactory);
    REQUIRE(SelectTextureSourceFactory("forest\\leaf.tga") == GTextureSourceJPEGFactory);
}

TEST_CASE("PNG: a blended alpha survives decode and is reported as alpha", "[Graphics][PNG][sinkhole]")
{
    TextureSourceJPEG src;
    PacLevelMem mips[kMaxMips];
    REQUIRE(src.Init(GET_FIXTURE("png/rgba_blend_32x32.png"), mips, kMaxMips));
    REQUIRE(src.GetFormat() == PacARGB8888);
    REQUIRE(src.IsAlpha());
    REQUIRE_FALSE(src.IsTransparent());

    mips[0].SetDestFormat(PacARGB8888, 8);
    std::vector<uint8_t> buf(32 * 32 * 4);
    REQUIRE(src.GetMipmapData(buf.data(), mips[0], 0));
    // Row 0: alpha ramps x*8, colour (200,40,40) stored B,G,R,A.
    REQUIRE(buf[0 * 4 + 3] == 0);
    REQUIRE(buf[16 * 4 + 3] == 128);
    REQUIRE(buf[16 * 4 + 2] == 200);
    REQUIRE(buf[16 * 4 + 0] == 40);
}

TEST_CASE("PNG: a 0/255 alpha is a punch-through cut-out", "[Graphics][PNG][sinkhole]")
{
    TextureSourceJPEG src;
    PacLevelMem mips[kMaxMips];
    REQUIRE(src.Init(GET_FIXTURE("png/rgba_cutout_32x32.png"), mips, kMaxMips));
    REQUIRE_FALSE(src.IsAlpha());
    REQUIRE(src.IsTransparent());
}

TEST_CASE("PNG: RGB and all-255 RGBA images stay opaque", "[Graphics][PNG][sinkhole]")
{
    for (const char* f : {"png/rgb_opaque_32x32.png", "png/rgba_opaque_32x32.png"})
    {
        TextureSourceJPEG src;
        PacLevelMem mips[kMaxMips];
        REQUIRE(src.Init(GET_FIXTURE(f), mips, kMaxMips));
        REQUIRE_FALSE(src.IsAlpha());
        REQUIRE_FALSE(src.IsTransparent());
        // ForceAlpha is a no-op: the renderers call it on every 32-bit format, and an
        // opaque PNG must not turn into a blended texture because of it.
        src.ForceAlpha();
        REQUIRE_FALSE(src.IsAlpha());
    }
}

// ---------------------------------------------------------------- DDS

TEST_CASE("DDS: A8R8G8B8 is read through its masks, not assumed to be RGBA", "[Graphics][DDS][sinkhole]")
{
    auto f = PlainHeader(1, 1, kRgb | kAlphaPixels, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    Append<uint8_t>(f, {0x30, 0x20, 0x10, 0x80}); // B,G,R,A in memory
    DDSFile dds = DDSConverter::ReadDDSBuffer(f.data(), f.size());
    REQUIRE(dds.valid());
    REQUIRE(dds.format == PixelFormat::RGBA8888);
    REQUIRE(dds.mipmaps[0].data == std::vector<uint8_t>{0x10, 0x20, 0x30, 0x80});
}

TEST_CASE("DDS: 24-bit, 16-bit 565, L8, A8L8 and A8 layouts decode", "[Graphics][DDS][sinkhole]")
{
    SECTION("R8G8B8")
    {
        auto f = PlainHeader(1, 1, kRgb, 0, 24, 0xFF0000, 0x00FF00, 0x0000FF, 0);
        Append<uint8_t>(f, {0x03, 0x02, 0x01});
        DDSFile d = DDSConverter::ReadDDSBuffer(f.data(), f.size());
        REQUIRE(d.valid());
        REQUIRE(d.mipmaps[0].data == std::vector<uint8_t>{0x01, 0x02, 0x03, 0xFF});
    }
    SECTION("R5G6B5")
    {
        auto f = PlainHeader(1, 1, kRgb, 0, 16, 0xF800, 0x07E0, 0x001F, 0);
        Append<uint16_t>(f, {0xF800}); // pure red
        DDSFile d = DDSConverter::ReadDDSBuffer(f.data(), f.size());
        REQUIRE(d.valid());
        REQUIRE(d.mipmaps[0].data == std::vector<uint8_t>{255, 0, 0, 255});
    }
    SECTION("L8")
    {
        auto f = PlainHeader(1, 1, kLuminance, 0, 8, 0xFF, 0, 0, 0);
        Append<uint8_t>(f, {0x7F});
        DDSFile d = DDSConverter::ReadDDSBuffer(f.data(), f.size());
        REQUIRE(d.valid());
        REQUIRE(d.mipmaps[0].data == std::vector<uint8_t>{0x7F, 0x7F, 0x7F, 0xFF});
    }
    SECTION("A8L8")
    {
        auto f = PlainHeader(1, 1, kLuminance | kAlphaPixels, 0, 16, 0x00FF, 0, 0, 0xFF00);
        Append<uint16_t>(f, {0x4020});
        DDSFile d = DDSConverter::ReadDDSBuffer(f.data(), f.size());
        REQUIRE(d.valid());
        REQUIRE(d.mipmaps[0].data == std::vector<uint8_t>{0x20, 0x20, 0x20, 0x40});
    }
    SECTION("A8")
    {
        auto f = PlainHeader(1, 1, kAlpha, 0, 8, 0, 0, 0, 0xFF);
        Append<uint8_t>(f, {0x55});
        DDSFile d = DDSConverter::ReadDDSBuffer(f.data(), f.size());
        REQUIRE(d.valid());
        REQUIRE(d.mipmaps[0].data == std::vector<uint8_t>{0xFF, 0xFF, 0xFF, 0x55});
    }
}

TEST_CASE("DDS: DXT2 and DXT4 load as DXT3 and DXT5", "[Graphics][DDS][sinkhole]")
{
    for (auto [cc, want] : {std::pair{"DXT2", PixelFormat::DXT3}, std::pair{"DXT4", PixelFormat::DXT5}})
    {
        auto f = PlainHeader(4, 4, kFourCC, FourCC(cc), 0, 0, 0, 0, 0);
        f.resize(f.size() + 16, 0); // one 16-byte block
        DDSFile d = DDSConverter::ReadDDSBuffer(f.data(), f.size());
        REQUIRE(d.valid());
        REQUIRE(d.format == want);
    }
}

TEST_CASE("DDS: a DXT1 that uses its punch-through colour is transparent", "[Graphics][DDS][sinkhole]")
{
    // Two 4x4 DXT1 files. Block: color0, color1, 32-bit indices.
    auto make = [](uint16_t c0, uint16_t c1, uint32_t idx)
    {
        auto f = PlainHeader(4, 4, kFourCC, FourCC("DXT1"), 0, 0, 0, 0, 0);
        Append<uint16_t>(f, {c0, c1});
        Append<uint32_t>(f, {idx});
        return f;
    };
    SECTION("3-colour block using index 3")
    {
        auto f = make(0x0000, 0xFFFF, 0xC0000000u); // last texel index 3
        TextureSourceDDS src;
        REQUIRE(src.InitFromMemory(f.data(), f.size(), "cut.dds"));
        REQUIRE(src.GetFormat() == PacDXT1);
        REQUIRE(src.IsTransparent());
    }
    SECTION("4-colour block is opaque")
    {
        auto f = make(0xFFFF, 0x0000, 0xFFFFFFFFu);
        TextureSourceDDS src;
        REQUIRE(src.InitFromMemory(f.data(), f.size(), "solid.dds"));
        REQUIRE_FALSE(src.IsTransparent());
    }
}
