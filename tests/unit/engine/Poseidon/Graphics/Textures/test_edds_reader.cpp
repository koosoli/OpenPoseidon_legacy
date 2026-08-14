// test_edds_reader.cpp - DZ-002: the Enfusion .edds container.
//
// Every container here is built byte by byte from the format, so nothing DayZ-owned
// is committed and the expected result is known by construction. The corpus check
// that matters lives outside CI, where the corpus is: all 454 .edds files in a
// retail DayZ install close their byte accounting exactly and every one of their
// 3,339 mip chunks decodes to the size its dimensions and block format require.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <Poseidon/Graphics/Textures/DdsImport.hpp>
#include <Poseidon/Graphics/Textures/EddsReader.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using Poseidon::IsEddsBuffer;
using Poseidon::PixelFormat;
using Poseidon::ReadEddsBuffer;

namespace
{

void PutU32(std::vector<uint8_t>& buffer, size_t offset, uint32_t value)
{
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

void AppendU32(std::vector<uint8_t>& buffer, uint32_t value)
{
    const size_t at = buffer.size();
    buffer.resize(at + 4);
    PutU32(buffer, at, value);
}

constexpr uint32_t kDdsdMipMapCount = 0x00020000;
constexpr uint32_t kDdpfAlphaPixels = 0x01;
constexpr uint32_t kDdpfRgb = 0x40;

// A 128-byte DDS header for a 32-bit BGRA image, carrying Enfusion's marker.
std::vector<uint8_t> Header(int width, int height, int mips, bool withMarker = true)
{
    std::vector<uint8_t> h(128, 0);
    std::memcpy(h.data(), "DDS ", 4);
    PutU32(h, 0x04, 124);              // dwSize
    PutU32(h, 0x08, kDdsdMipMapCount); // dwFlags
    PutU32(h, 0x0C, static_cast<uint32_t>(height));
    PutU32(h, 0x10, static_cast<uint32_t>(width));
    PutU32(h, 0x1C, static_cast<uint32_t>(mips));
    if (withMarker)
        std::memcpy(h.data() + 0x24, "ENF1", 4); // dwReserved1[1]
    PutU32(h, 0x4C, 32);                         // ddspf.dwSize
    PutU32(h, 0x50, kDdpfRgb | kDdpfAlphaPixels);
    PutU32(h, 0x58, 32); // bits per pixel
    PutU32(h, 0x5C, 0x00FF0000);
    PutU32(h, 0x60, 0x0000FF00);
    PutU32(h, 0x64, 0x000000FF);
    PutU32(h, 0x68, 0xFF000000);
    return h;
}

// A 4x4 BGRA image with three levels, every level filled with a distinct byte so
// the reader's level ordering is observable in the data and not just the extents.
// The chunk table runs smallest level first, which is the whole point.
std::vector<uint8_t> ThreeLevelCopyImage(uint8_t fill1x1 = 0x11, uint8_t fill2x2 = 0x22, uint8_t fill4x4 = 0x44)
{
    std::vector<uint8_t> file = Header(4, 4, 3);
    AppendU32(file, 0x59504F43); // 'COPY'
    AppendU32(file, 4);          // 1x1 x 4 bytes
    AppendU32(file, 0x59504F43);
    AppendU32(file, 16); // 2x2
    AppendU32(file, 0x59504F43);
    AppendU32(file, 64); // 4x4
    file.insert(file.end(), 4, fill1x1);
    file.insert(file.end(), 16, fill2x2);
    file.insert(file.end(), 64, fill4x4);
    return file;
}

} // namespace

TEST_CASE("EDDS: the marker distinguishes it from a plain DDS", "[graphics][edds][dz-002]")
{
    const std::vector<uint8_t> marked = ThreeLevelCopyImage();
    REQUIRE(IsEddsBuffer(marked.data(), marked.size()));

    std::vector<uint8_t> plain = marked;
    std::memset(plain.data() + 0x24, 0, 4);
    REQUIRE_FALSE(IsEddsBuffer(plain.data(), plain.size()));

    // And reading it as an .edds says so rather than producing garbage, because the
    // header alone parses fine either way.
    const auto image = ReadEddsBuffer(plain.data(), plain.size());
    REQUIRE_FALSE(image.valid());
    REQUIRE(image.error.find("ENF1") != std::string::npos);
}

TEST_CASE("EDDS: the chunk table is smallest level first", "[graphics][edds][dz-002]")
{
    // Read the other way round every texture is a 1x1 smear -- a bug that renders
    // rather than one that errors, so it is asserted on the data, not the extents.
    const std::vector<uint8_t> file = ThreeLevelCopyImage();
    const auto image = ReadEddsBuffer(file.data(), file.size());

    REQUIRE(image.valid());
    REQUIRE(image.error.empty());
    REQUIRE(image.width == 4);
    REQUIRE(image.height == 4);
    REQUIRE(image.format == PixelFormat::ARGB8888);
    REQUIRE(image.hasAlpha);
    REQUIRE(image.mipmaps.size() == 3);

    REQUIRE(image.mipmaps[0].width == 4);
    REQUIRE(image.mipmaps[0].data.size() == 64);
    REQUIRE(image.mipmaps[0].data[0] == 0x44);

    REQUIRE(image.mipmaps[1].width == 2);
    REQUIRE(image.mipmaps[1].data.size() == 16);
    REQUIRE(image.mipmaps[1].data[0] == 0x22);

    REQUIRE(image.mipmaps[2].width == 1);
    REQUIRE(image.mipmaps[2].data.size() == 4);
    REQUIRE(image.mipmaps[2].data[0] == 0x11);
}

TEST_CASE("EDDS: a stored chunk must be exactly its natural size", "[graphics][edds][dz-002]")
{
    // This is the check that catches a field read at the wrong width anywhere
    // earlier in the header, which is how the container was fitted in the first
    // place. Shrink the largest level's declared size and it must not be accepted.
    std::vector<uint8_t> file = ThreeLevelCopyImage();
    PutU32(file, 128 + 5 * 4, 60); // the 4x4 chunk's size field
    file.resize(file.size() - 4);

    const auto image = ReadEddsBuffer(file.data(), file.size());
    REQUIRE_FALSE(image.valid());
    REQUIRE(image.error.find("requires") != std::string::npos);
}

TEST_CASE("EDDS: byte accounting must close on the file size", "[graphics][edds][dz-002]")
{
    std::vector<uint8_t> file = ThreeLevelCopyImage();
    file.push_back(0); // one trailing byte nothing accounts for

    const auto image = ReadEddsBuffer(file.data(), file.size());
    REQUIRE_FALSE(image.valid());
    REQUIRE(image.error.find("accounting") != std::string::npos);
}

TEST_CASE("EDDS: an unknown chunk tag is refused by name", "[graphics][edds][dz-002]")
{
    std::vector<uint8_t> file = ThreeLevelCopyImage();
    std::memcpy(file.data() + 128, "ZIP\0", 4); // the smallest level's tag

    const auto image = ReadEddsBuffer(file.data(), file.size());
    REQUIRE_FALSE(image.valid());
    REQUIRE(image.error.find("chunk tag") != std::string::npos);
}

TEST_CASE("EDDS: a compressed level decodes through its sub-block chain", "[graphics][edds][dz-002]")
{
    // One 1x1 BGRA level stored as 'LZ4 ': [u32 rawTotal][u32 compSize|final][block].
    // The block is four literals, which is the smallest well-formed LZ4 there is.
    std::vector<uint8_t> file = Header(1, 1, 1);
    const uint32_t chunkSize = 4 + 4 + 5; // rawTotal + sub-block header + block
    AppendU32(file, 0x20345A4C);          // 'LZ4 '
    AppendU32(file, chunkSize);
    AppendU32(file, 4);               // rawTotal
    AppendU32(file, 0x80000000u | 5); // final sub-block, 5 compressed bytes
    file.push_back(0x40);             // token: 4 literals, no match
    file.push_back(0xDE);
    file.push_back(0xAD);
    file.push_back(0xBE);
    file.push_back(0xEF);

    const auto image = ReadEddsBuffer(file.data(), file.size());
    REQUIRE(image.valid());
    REQUIRE(image.mipmaps.size() == 1);
    REQUIRE(image.mipmaps[0].data == std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF});
}

TEST_CASE("EDDS: a compressed level that decodes short is refused", "[graphics][edds][dz-002]")
{
    std::vector<uint8_t> file = Header(1, 1, 1);
    const uint32_t chunkSize = 4 + 4 + 3;
    AppendU32(file, 0x20345A4C); // 'LZ4 '
    AppendU32(file, chunkSize);
    AppendU32(file, 4); // claims 4 raw bytes
    AppendU32(file, 0x80000000u | 3);
    file.push_back(0x20); // token: only 2 literals
    file.push_back(0xDE);
    file.push_back(0xAD);

    const auto image = ReadEddsBuffer(file.data(), file.size());
    REQUIRE_FALSE(image.valid());
    REQUIRE(image.error.find("expected") != std::string::npos);
}

TEST_CASE("EDDS: the DX10 header routes BC5 onto a real decoder", "[graphics][edds][arf-001]")
{
    // This used to assert that BC5 was REFUSED, which was true when the corpus was
    // DayZ's 454 .edds and only 23 of them were DXGI-era. Reforger inverted that
    // ratio -- BC7 alone was 87% of everything the reader could not decode -- so
    // BC4/BC5/BC7 are now decoded and the DX10 path must select the format rather
    // than name a gap (ARF-001).
    std::vector<uint8_t> file = Header(4, 4, 1);
    PutU32(file, 0x50, 0x04); // DDPF_FOURCC
    std::memcpy(file.data() + 0x54, "DX10", 4);
    // The DXT10 header is 20 bytes, not just the format word: dxgiFormat,
    // resourceDimension, miscFlag, arraySize, miscFlags2.
    AppendU32(file, 83); // DXGI_FORMAT_BC5_UNORM
    AppendU32(file, 3);  // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    AppendU32(file, 0);
    AppendU32(file, 1); // arraySize
    AppendU32(file, 0);
    AppendU32(file, 0x59504F43); // 'COPY'
    AppendU32(file, 16);         // one 4x4 BC5 block
    file.resize(file.size() + 16);

    const auto image = ReadEddsBuffer(file.data(), file.size());
    REQUIRE(image.valid());
    REQUIRE(image.format == PixelFormat::BC5);
}

TEST_CASE("EDDS: a still-unsupported DXGI format is named, not swallowed", "[graphics][edds][arf-001]")
{
    // BC6H is the remaining named gap: it is HDR, it does not occur in the sampled
    // Reforger corpus, and a decoder with no input to check it against is a
    // liability rather than coverage. What matters is that the refusal says which
    // format it refused.
    std::vector<uint8_t> file = Header(4, 4, 1);
    PutU32(file, 0x50, 0x04); // DDPF_FOURCC
    std::memcpy(file.data() + 0x54, "DX10", 4);
    AppendU32(file, 95); // DXGI_FORMAT_BC6H_UF16
    file.resize(file.size() + 16);

    const auto image = ReadEddsBuffer(file.data(), file.size());
    REQUIRE_FALSE(image.valid());
    REQUIRE(image.error.find("BC6H") != std::string::npos);
}

TEST_CASE("EDDS: both extensions reach the DDS texture source", "[graphics][edds][dz-002]")
{
    // The registration half of the fix, asserted separately because nothing in the
    // engine requests an .edds yet -- that only happens once a material reader asks
    // for one. Until then a working reader that the selector never reaches would
    // look exactly like a working feature.
    REQUIRE(Poseidon::SelectTextureSourceFactory("dz/water/data/river_nohq.edds") ==
            Poseidon::GTextureSourceDDSFactory);
    REQUIRE(Poseidon::SelectTextureSourceFactory("graphics/textures/sky.dds") == Poseidon::GTextureSourceDDSFactory);
    // Case-insensitively, as every other extension here is matched.
    REQUIRE(Poseidon::SelectTextureSourceFactory("DZ/WATER/RIVER_NOHQ.EDDS") == Poseidon::GTextureSourceDDSFactory);

    // And the extensions that were already handled still go where they went.
    REQUIRE(Poseidon::SelectTextureSourceFactory("data/wall.paa") != Poseidon::GTextureSourceDDSFactory);
    REQUIRE(Poseidon::SelectTextureSourceFactory("data/wall.paa") != nullptr);
}
