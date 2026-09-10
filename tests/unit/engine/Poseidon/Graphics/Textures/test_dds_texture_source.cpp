// test_dds_texture_source.cpp - DZ-002: TextureSourceDDS, the ITextureSource over .edds.
//
// The container itself is tested in test_edds_reader.cpp. What is tested here is the part
// that hands decoded levels to the renderer, because that is where a texture that parsed
// perfectly can still arrive wrong: the wrong destination format, a row pitch ignored, or
// a block format quietly converted.
//
// The 32-bit path matters more than its share of the corpus suggests. DayZ's water
// environment map -- the texture its whole appearance depends on (DZ-003) -- is 32-bit
// BGRA, and until reflection is bound nothing in the game exercises it.
//
// Inputs are built byte by byte from the format: DayZ's data is proprietary and no fixture
// may be committed.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Textures/DdsImport.hpp>
#include <Poseidon/Graphics/Textures/EnfusionTextureName.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using Poseidon::PacARGB1555;
using Poseidon::PacARGB8888;
using Poseidon::PacDXT1;
using Poseidon::PacLevelMem;
using Poseidon::PacRGB565;
using Poseidon::TextureSourceDDS;

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

// 128-byte DDS header carrying Enfusion's marker. `fourCC` empty means a 32-bit BGRA
// layout; otherwise a block format.
std::vector<uint8_t> Header(int width, int height, int mips, const char* fourCC = nullptr)
{
    std::vector<uint8_t> h(128, 0);
    std::memcpy(h.data(), "DDS ", 4);
    PutU32(h, 0x04, 124);
    PutU32(h, 0x08, 0x00020000); // DDSD_MIPMAPCOUNT
    PutU32(h, 0x0C, static_cast<uint32_t>(height));
    PutU32(h, 0x10, static_cast<uint32_t>(width));
    PutU32(h, 0x1C, static_cast<uint32_t>(mips));
    std::memcpy(h.data() + 0x24, "ENF1", 4);
    PutU32(h, 0x4C, 32);
    if (fourCC)
    {
        PutU32(h, 0x50, 0x04); // DDPF_FOURCC
        std::memcpy(h.data() + 0x54, fourCC, 4);
    }
    else
    {
        PutU32(h, 0x50, 0x41); // DDPF_RGB | DDPF_ALPHAPIXELS
        PutU32(h, 0x58, 32);
        PutU32(h, 0x5C, 0x00FF0000);
        PutU32(h, 0x60, 0x0000FF00);
        PutU32(h, 0x64, 0x000000FF);
        PutU32(h, 0x68, 0xFF000000);
    }
    return h;
}

// A single-level 2x2 BGRA image whose four texels are distinguishable.
std::vector<uint8_t> Bgra2x2()
{
    std::vector<uint8_t> file = Header(2, 2, 1);
    AppendU32(file, 0x59504F43); // 'COPY'
    AppendU32(file, 16);
    const uint8_t texels[16] = {
        0x10, 0x20, 0x30, 0xFF, // B,G,R,A
        0x11, 0x21, 0x31, 0xFE, //
        0x12, 0x22, 0x32, 0xFD, //
        0x13, 0x23, 0x33, 0xFC,
    };
    file.insert(file.end(), texels, texels + 16);
    return file;
}

} // namespace

TEST_CASE("Enfusion NTC normals use red rather than cavity for X", "[graphics][edds][ntc]")
{
    REQUIRE(Poseidon::IsEnfusionRgNormalName("assets/polyplane_picea_NTC.edds"));
    REQUIRE(Poseidon::IsEnfusionRgNormalName("assets/bark_picea_ntc.edds"));
    REQUIRE(Poseidon::IsEnfusionRgNormalName("assets/wall_nmo.edds"));
    REQUIRE_FALSE(Poseidon::IsEnfusionRgNormalName("assets/wall_nohq.paa"));
    REQUIRE_FALSE(Poseidon::IsEnfusionRgNormalName("assets/leaf_bcr.edds"));
    REQUIRE_FALSE(Poseidon::IsEnfusionRgNormalName("assets/leaf_a.edds"));
    REQUIRE_FALSE(Poseidon::IsEnfusionRgNormalName("assets/old_ntc/leaf_bcr.edds"));
    REQUIRE_FALSE(Poseidon::IsEnfusionRgNormalName("assets/leaf_ntc_bcr.edds"));
    REQUIRE_FALSE(Poseidon::IsEnfusionRgNormalName(nullptr));

    // Synthetic BC7 mode 6, uniform RGBA=(128,128,40,200). No game assets.
    uint8_t block[16] = {};
    unsigned bit = 0;
    auto write = [&](unsigned value, unsigned bits) {
        for (unsigned i = 0; i < bits; ++i, ++bit)
            block[bit / 8] |= ((value >> i) & 1u) << (bit % 8);
    };
    write(1u << 6, 7);
    for (unsigned value : {64u, 64u, 20u, 100u}) { write(value, 7); write(value, 7); }
    write(0, 1); write(0, 1); write(0, 3);
    for (int i = 1; i < 16; ++i) write(0, 4);
    REQUIRE(bit == 128);
    auto file = Header(4, 4, 1, "DX10");
    for (uint32_t word : {98u, 3u, 0u, 1u, 0u, 0x59504F43u, 16u}) AppendU32(file, word);
    file.insert(file.end(), block, block + 16);
    for (const char* name : {"test_ntc.edds", "test_nmo.edds", "test_bcr.edds", "test_coverage.edds"})
    {
        INFO(name);
        TextureSourceDDS source;
        REQUIRE(source.InitFromMemory(file.data(), file.size(), name, true));
        PacLevelMem mip;
        mip._w = mip._h = 4; mip._pitch = 16; mip._dFormat = PacARGB8888;
        uint8_t pixels[64] = {};
        REQUIRE(source.GetMipmapData(pixels, mip, 0));
        for (int i = 0; i < 64; i += 4)
        {
            REQUIRE(pixels[i + 2] == 128);
            REQUIRE(pixels[i + 1] == 128);
            // BCR roughness is deliberately not forwarded as coverage by this
            // existing source path. An ordinary RGBA image retains its alpha.
            const unsigned alpha = Poseidon::IsEnfusionRgNormalName(name) ? 128 :
                                   (std::strcmp(name, "test_bcr.edds") == 0 ? 255 : 200);
            REQUIRE(static_cast<unsigned>(pixels[i + 3]) == alpha);
        }
    }
}

TEST_CASE("DDS source: a 32-bit level is delivered as BGRA, honouring pitch", "[graphics][edds][dz-002]")
{
    const std::vector<uint8_t> file = Bgra2x2();
    TextureSourceDDS source;
    REQUIRE(source.InitFromMemory(file.data(), file.size(), "test.edds"));
    REQUIRE(source.GetMipmapCount() == 1);
    REQUIRE(source.GetFormat() == PacARGB8888);

    PacLevelMem mip;
    mip._w = 2;
    mip._h = 2;
    mip._dFormat = PacARGB8888;

    SECTION("tight rows")
    {
        mip._pitch = 8; // 2 texels x 4 bytes
        uint8_t out[16] = {};
        REQUIRE(source.GetMipmapData(out, mip, 0));
        // Byte order is preserved: the renderer's ARGB8888 is B,G,R,A in memory.
        REQUIRE(out[0] == 0x10);
        REQUIRE(out[3] == 0xFF);
        REQUIRE(out[12] == 0x13);
    }

    SECTION("padded rows")
    {
        // A destination row wider than the source is the case that a plain memcpy of the
        // whole level gets wrong: row 1 would land 8 bytes early and the image would
        // shear. 0xCC marks the padding so an overrun is visible.
        mip._pitch = 12;
        uint8_t out[24];
        std::memset(out, 0xCC, sizeof(out));
        REQUIRE(source.GetMipmapData(out, mip, 0));

        REQUIRE(out[0] == 0x10); // row 0, texel 0
        REQUIRE(out[4] == 0x11); // row 0, texel 1
        REQUIRE(out[8] == 0xCC); // row 0 padding, untouched
        REQUIRE(out[9] == 0xCC);
        REQUIRE(out[12] == 0x12); // row 1 starts at the pitch, not at 8
        REQUIRE(out[16] == 0x13);
        REQUIRE(out[20] == 0xCC); // row 1 padding, untouched
    }
}

TEST_CASE("DDS source: 32-bit converts down to the 16-bit destinations", "[graphics][edds][dz-002]")
{
    const std::vector<uint8_t> file = Bgra2x2();
    TextureSourceDDS source;
    REQUIRE(source.InitFromMemory(file.data(), file.size(), "test.edds"));

    PacLevelMem mip;
    mip._w = 2;
    mip._h = 2;
    mip._pitch = 4;

    SECTION("ARGB1555 keeps the high bits and thresholds alpha")
    {
        mip._dFormat = PacARGB1555;
        uint16_t out[4] = {};
        REQUIRE(source.GetMipmapData(out, mip, 0));
        // Texel 0 is B=0x10 G=0x20 R=0x30 A=0xFF -> (1, 0b00110, 0b00100, 0b00010).
        REQUIRE((out[0] & 0x8000) != 0);          // alpha 0xFF is above the threshold
        REQUIRE(((out[0] >> 10) & 0x1F) == 0x06); // R 0x30 >> 3
        REQUIRE(((out[0] >> 5) & 0x1F) == 0x04);  // G 0x20 >> 3
        REQUIRE((out[0] & 0x1F) == 0x02);         // B 0x10 >> 3
    }

    SECTION("RGB565 gives green its extra bit")
    {
        mip._dFormat = PacRGB565;
        uint16_t out[4] = {};
        REQUIRE(source.GetMipmapData(out, mip, 0));
        REQUIRE(((out[0] >> 11) & 0x1F) == 0x06); // R
        REQUIRE(((out[0] >> 5) & 0x3F) == 0x08);  // G 0x20 >> 2
        REQUIRE((out[0] & 0x1F) == 0x02);         // B
    }
}

TEST_CASE("DDS source: block levels transfer verbatim and only to their own format", "[graphics][edds][dz-002]")
{
    // Blocks are deliberately NOT decompressed on this path: DayZ worlds hold ~28,000
    // textures at 1k-4k and inflating each one 8x on the way in is the one thing this
    // must not do (DZ-002). So a DXT level is a memcpy, and a mismatched destination is
    // an error rather than a silent conversion.
    std::vector<uint8_t> file = Header(4, 4, 1, "DXT1");
    AppendU32(file, 0x59504F43); // 'COPY'
    AppendU32(file, 8);          // one 4x4 DXT1 block
    const uint8_t block[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    file.insert(file.end(), block, block + 8);

    TextureSourceDDS source;
    REQUIRE(source.InitFromMemory(file.data(), file.size(), "block.edds"));
    REQUIRE(source.GetFormat() == PacDXT1);

    PacLevelMem mip;
    mip._w = 4;
    mip._h = 4;
    mip._dFormat = PacDXT1;
    uint8_t out[8] = {};
    REQUIRE(source.GetMipmapData(out, mip, 0));
    REQUIRE(std::memcmp(out, block, 8) == 0);

    // Asking for it as anything else must fail rather than produce rubbish.
    mip._dFormat = PacARGB8888;
    uint8_t wide[64] = {};
    REQUIRE_FALSE(source.GetMipmapData(wide, mip, 0));
}

TEST_CASE("DDS source: a level whose extents disagree with the caller is refused", "[graphics][edds][dz-002]")
{
    // The mip table and the caller's PacLevelMem are two independent descriptions of the
    // same level. If they disagree, writing anyway would overrun or shear.
    const std::vector<uint8_t> file = Bgra2x2();
    TextureSourceDDS source;
    REQUIRE(source.InitFromMemory(file.data(), file.size(), "test.edds"));

    PacLevelMem mip;
    mip._w = 4; // the file says 2
    mip._h = 2;
    mip._pitch = 16;
    mip._dFormat = PacARGB8888;
    uint8_t out[32] = {};
    REQUIRE_FALSE(source.GetMipmapData(out, mip, 0));

    // And a level index past the end is refused too.
    mip._w = 2;
    mip._pitch = 8;
    REQUIRE_FALSE(source.GetMipmapData(out, mip, 1));
    REQUIRE_FALSE(source.GetMipmapData(out, mip, -1));
}

TEST_CASE("DDS source: mip 0 is the largest level", "[graphics][edds][dz-002]")
{
    // The on-disk chunk table runs smallest-first; a source that forwarded that order
    // would hand the renderer a 1x1 smear as its base level.
    std::vector<uint8_t> file = Header(2, 2, 2);
    AppendU32(file, 0x59504F43);
    AppendU32(file, 4); // 1x1
    AppendU32(file, 0x59504F43);
    AppendU32(file, 16); // 2x2
    file.insert(file.end(), 4, 0x77);
    file.insert(file.end(), 16, 0x22);

    TextureSourceDDS source;
    REQUIRE(source.InitFromMemory(file.data(), file.size(), "mips.edds"));
    REQUIRE(source.GetMipmapCount() == 2);

    PacLevelMem mip;
    mip._w = 2;
    mip._h = 2;
    mip._pitch = 8;
    mip._dFormat = PacARGB8888;
    uint8_t out[16] = {};
    REQUIRE(source.GetMipmapData(out, mip, 0));
    REQUIRE(out[0] == 0x22); // the 2x2 fill, not the 1x1 one
}

// ---------------------------------------------------------------------------------
// RFG-070: the `enft|r,g,b|` layer tint.
//
// Everon's buildings read as one pale grey because a `MatPBRMulti` names no albedo of its
// own -- it names a `BCR_N`, which is a SHARED one-metre library tile, and 436 of them
// serve 2,889 of the island's structure materials. The per-layer `Color_N` beside the tile
// is what makes a wall its own colour, and the native path threw it away.
//
// Two things are pinned here and they are the two that fail silently.

TEST_CASE("Enfusion layer tint: r,g,b lands in R,G,B and not in B,G,R", "[graphics][edds][rfg-070]")
{
    // The trap: decoded levels are ARGB8888, which in this engine means B,G,R,A in memory.
    // A tint applied in index order paints a beige wall blue -- and it still LOOKS like a
    // working tint, because the image does change colour. Only a per-channel assertion
    // catches it, so the target here is deliberately lopsided: strongly red, barely blue.
    std::vector<Poseidon::DDSMipLevel> levels(1);
    levels[0].width = 2;
    levels[0].height = 2;
    levels[0].data.assign(2 * 2 * 4, 0);
    for (size_t t = 0; t < 4; ++t)
    {
        levels[0].data[t * 4 + 0] = 128; // B
        levels[0].data[t * 4 + 1] = 128; // G
        levels[0].data[t * 4 + 2] = 128; // R
        levels[0].data[t * 4 + 3] = 200; // A -- roughness on a _BCR; must survive untouched
    }
    const float rgb[3] = {0.9f, 0.5f, 0.05f}; // linear
    Poseidon::ApplyEnfusionLayerTint(levels, rgb);

    const uint8_t b = levels[0].data[0];
    const uint8_t g = levels[0].data[1];
    const uint8_t r = levels[0].data[2];
    REQUIRE(r > g);
    REQUIRE(g > b);
    // sRGB(0.9) ~ 0.955 -> ~244; sRGB(0.05) ~ 0.2416 -> ~62. Loose bounds: what is being
    // pinned is which byte moved where, not the encoder's last digit.
    REQUIRE(r > 230);
    REQUIRE(b < 90);
    // Alpha is coverage or roughness and is never the tint's business.
    REQUIRE(levels[0].data[3] == 200);
}

TEST_CASE("Enfusion layer tint: one gain for the whole mip chain", "[graphics][edds][rfg-070]")
{
    // A per-level mean would give each mip its own gain, and the same wall would change
    // colour as you walked away from it. The gain comes from level 0 and is applied to all.
    std::vector<Poseidon::DDSMipLevel> levels(2);
    levels[0].width = 2;
    levels[0].height = 2;
    levels[0].data.assign(2 * 2 * 4, 100);
    levels[1].width = 1;
    levels[1].height = 1;
    levels[1].data.assign(4, 100);
    const float rgb[3] = {0.25f, 0.25f, 0.25f};
    Poseidon::ApplyEnfusionLayerTint(levels, rgb);
    REQUIRE(levels[0].data[0] == levels[1].data[0]);
    REQUIRE(levels[0].data[2] == levels[1].data[2]);
}

TEST_CASE("Enfusion layer tint: a zero channel is left alone", "[graphics][edds][rfg-070]")
{
    // gain = target / mean, and a tile with a dead channel would be multiplied by infinity.
    std::vector<Poseidon::DDSMipLevel> levels(1);
    levels[0].width = 1;
    levels[0].height = 1;
    levels[0].data = {0, 60, 60, 255}; // B dead
    const float rgb[3] = {0.5f, 0.5f, 0.5f};
    Poseidon::ApplyEnfusionLayerTint(levels, rgb);
    REQUIRE(levels[0].data[0] == 0);
    REQUIRE(levels[0].data[2] > 60);
}

TEST_CASE("Enfusion composite names nest: tint outside coverage", "[graphics][edds][rfg-070]")
{
    // The claim this pins is COMPOSABILITY. A leaf card on a tinted shared tile is both
    // forms at once, and three separate places in the renderer test `strncmp(name,
    // "enfa|", 5)` by hand to decide whether a section is a cutout, whether the material's
    // Stage0 may override the face texture, and which pass it draws in. Wrapping a tint
    // around the name must not change any of those three answers -- so every one of them
    // reads through SkipLayerTint, and this is the test that says what that has to do.
    const float rgb[3] = {0.385f, 0.356f, 0.322f}; // the corpus median Color_N
    const std::string inner = "enfa|assets/x_A.edds|assets/x_BCR.edds";
    const std::string name = Poseidon::Enfusion::MakeLayerTintName(rgb, inner);

    REQUIRE(name.rfind("enft|", 0) == 0);
    REQUIRE(std::string(Poseidon::Enfusion::SkipLayerTint(name.c_str())) == inner);
    REQUIRE(std::strncmp(Poseidon::Enfusion::SkipLayerTint(name.c_str()), "enfa|", 5) == 0);

    float back[3] = {0.0f, 0.0f, 0.0f};
    const char* rest = nullptr;
    REQUIRE(Poseidon::Enfusion::SplitLayerTintName(name.c_str(), back, &rest));
    REQUIRE(std::string(rest) == inner);
    for (int c = 0; c < 3; ++c)
        REQUIRE(std::abs(back[c] - rgb[c]) < 1e-3f);

    // An untinted name passes through untouched, which is what keeps every non-Enfusion
    // world inert: nothing else in the engine ever builds one of these.
    const char* plain = "data\wall.paa";
    REQUIRE(Poseidon::Enfusion::SkipLayerTint(plain) == plain);
    REQUIRE(!Poseidon::Enfusion::SplitLayerTintName(plain, back, &rest));
    REQUIRE(!Poseidon::Enfusion::SplitLayerTintName("enft|0.5,0.5,0.5", back, &rest)); // no inner
}
