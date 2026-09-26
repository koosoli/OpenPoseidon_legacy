// Classifying a texture's alpha off its compressed blocks instead of a decoded image.
//
// TextureWgpu::GetAlphaClass has to tell cutout from blend, and for a multi-bit-alpha
// format that needs the actual texel alphas. It used to get them by re-opening the file the
// upload had just read, decoding the full-resolution mip to RGBA8, and then reading one byte
// in four — three quarters of the decode existed to be thrown away. Measured on Reforger
// Everon it was 357 ms of the 896 ms of all texture streaming work, at 3.19 ms a call.
//
// It now reads the alpha straight out of the BC2/BC3 blocks. That is only legitimate if the
// alpha bytes are IDENTICAL to the ones the full decode produced — the classification
// thresholds are untouched, so identical alpha means an identical verdict, and anything less
// means fences and foliage silently changing pass.
//
// So that is what is pinned here: the same PAA bytes, through the shipped whole-file decoder
// and through the block path, must agree on every field of AlphaStats. The synthetic blocks
// exist because the checked-in fixtures are uniform (all-128 and all-255 alpha) and could not
// distinguish a right expansion from a wrong one; they cover BC3's two palette modes, its
// 0/255 terminals, BC2's nibble expansion, and the all-clear case ClassifyAlpha deliberately
// re-routes to Opaque.
//
// The block walk is a shared production helper used by TextureWgpu and these tests.
// Compare it with the independent whole-file decoder, including every alpha byte at edges.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <Poseidon/Graphics/Core/MipmapLayout.hpp>
#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <Poseidon/Graphics/Textures/BlockCompression.hpp>
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp> // MAX_MIPMAPS

#include "../test_fixtures.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>
#include <utility>
#include <limits>

using namespace Poseidon;

namespace
{

// ---------------------------------------------------------------------------------------
// The production helper, not a second implementation of its block walk.
AlphaStats AlphaFromBlocks(const uint8_t* blocks, int w, int h, bool bc3)
{
    std::vector<uint8_t> alpha;
    const size_t bytes = ((static_cast<size_t>(w) + 3) / 4) *
                         ((static_cast<size_t>(h) + 3) / 4) * 16;
    REQUIRE(DecodeBlockAlpha(blocks, bytes, w, h, bc3, alpha));
    REQUIRE(alpha.size() == static_cast<size_t>(w) * static_cast<size_t>(h));
    return ClassifyAlphaSamples(alpha.data(), alpha.size(), 1);
}

// Every field, not just the verdict. A kind-only comparison would pass on alpha values that
// happen to land the same side of a 2% threshold while being wrong everywhere else.
void RequireSameStats(const AlphaStats& reference, const AlphaStats& fromBlocks)
{
    CHECK(fromBlocks.kind == reference.kind);
    CHECK(fromBlocks.aMin == reference.aMin);
    CHECK(fromBlocks.aMax == reference.aMax);
    CHECK(fromBlocks.aMean == reference.aMean);
    CHECK(fromBlocks.pctClear == reference.pctClear);
    CHECK(fromBlocks.pctOpaque == reference.pctOpaque);
    CHECK(fromBlocks.pctPartial == reference.pctPartial);
    CHECK(fromBlocks.pctMid == reference.pctMid);
}

// ---------------------------------------------------------------------------------------
// A minimal in-memory PAA around hand-built blocks: magic, no TAGGs, an empty palette, one
// uncompressed level, terminator. This is the smallest thing DecodePAABuffer will accept, so
// the reference side of every synthetic case below is the real shipped decoder rather than a
// second copy of the arithmetic under test.
std::vector<char> BuildDxtPaa(uint16_t magic, int w, int h, const std::vector<uint8_t>& blocks)
{
    std::vector<char> out;
    auto put16 = [&out](int v)
    {
        out.push_back(static_cast<char>(v & 0xFF));
        out.push_back(static_cast<char>((v >> 8) & 0xFF));
    };
    auto put24 = [&out](int v)
    {
        out.push_back(static_cast<char>(v & 0xFF));
        out.push_back(static_cast<char>((v >> 8) & 0xFF));
        out.push_back(static_cast<char>((v >> 16) & 0xFF));
    };
    put16(magic);
    put16(0); // palette: zero colours
    put16(w);
    put16(h);
    put24(static_cast<int>(blocks.size()));
    out.insert(out.end(), blocks.begin(), blocks.end());
    put16(0); // mip-chain terminator
    put16(0);
    return out;
}

// One BC3 block: two alpha endpoints, sixteen 3-bit indices, then a colour block the alpha
// path must never look at (deliberately not flat, so a decoder that mixed the halves up
// would show).
std::vector<uint8_t> Bc3Block(uint8_t a0, uint8_t a1, const int idx[16])
{
    std::vector<uint8_t> b(16, 0);
    b[0] = a0;
    b[1] = a1;
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++)
    {
        bits |= static_cast<uint64_t>(idx[i] & 7) << (i * 3);
    }
    for (int i = 0; i < 6; i++)
    {
        b[2 + i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xFF);
    }
    b[8] = 0xFF;
    b[9] = 0xFF; // c0 = white
    b[10] = 0x00;
    b[11] = 0x00; // c1 = black
    b[12] = 0x1B;
    b[13] = 0x1B;
    b[14] = 0x1B;
    b[15] = 0x1B;
    return b;
}

// One BC2 block: sixteen explicit 4-bit alphas (low nibble first), then a colour block.
std::vector<uint8_t> Bc2Block(const int alpha4[16])
{
    std::vector<uint8_t> b(16, 0);
    for (int i = 0; i < 16; i += 2)
    {
        b[i / 2] = static_cast<uint8_t>((alpha4[i] & 0xF) | ((alpha4[i + 1] & 0xF) << 4));
    }
    b[8] = 0xFF;
    b[9] = 0xFF;
    b[10] = 0x00;
    b[11] = 0x00;
    b[12] = 0x1B;
    b[13] = 0x1B;
    b[14] = 0x1B;
    b[15] = 0x1B;
    return b;
}

void Append(std::vector<uint8_t>& into, const std::vector<uint8_t>& block)
{
    into.insert(into.end(), block.begin(), block.end());
}

// Reference: the whole file, through the decoder TextureWgpu used to call.
AlphaStats WholeFileStats(const std::vector<char>& paa)
{
    const DecodedImage img = DecodePAABuffer(paa.data(), paa.size(), true);
    REQUIRE(img.valid());
    return ClassifyAlpha(img.rgba.data(), static_cast<size_t>(img.width) * static_cast<size_t>(img.height));
}

constexpr uint16_t kDxt3Magic = 0xFF03;
constexpr uint16_t kDxt5Magic = 0xFF05;

} // namespace

TEST_CASE("BC3 alpha read off the blocks classifies as the full decode does", "[graphics][texture]")
{
    // 8x8 = four blocks, chosen so one file exercises both BC3 alpha palette modes and the
    // index range that reaches their ends.
    const int flat[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const int spread[16] = {0, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 1, 0};
    const int terminals[16] = {6, 7, 6, 7, 6, 7, 6, 7, 6, 7, 6, 7, 6, 7, 6, 7};

    std::vector<uint8_t> blocks;
    Append(blocks, Bc3Block(255, 0, flat));      // a0 > a1: six interpolants, index 0 -> 255
    Append(blocks, Bc3Block(0, 255, terminals)); // a0 <= a1: indices 6 and 7 are 0 and 255
    Append(blocks, Bc3Block(255, 0, spread));    // the whole six-interpolant ramp
    Append(blocks, Bc3Block(200, 100, spread));  // endpoints that are neither 0 nor 255

    const std::vector<char> paa = BuildDxtPaa(kDxt5Magic, 8, 8, blocks);
    const AlphaStats reference = WholeFileStats(paa);

    // Not a vacuous comparison: this image has clear, opaque AND partial texels, so all
    // three of ClassifyAlpha's counters are being compared against something.
    REQUIRE(reference.pctClear > 0.0);
    REQUIRE(reference.pctOpaque > 0.0);
    REQUIRE(reference.pctPartial > 0.0);

    RequireSameStats(reference, AlphaFromBlocks(blocks.data(), 8, 8, true));
}

TEST_CASE("BC3 punch-through holes still classify as cutout off the blocks", "[graphics][texture]")
{
    // The case the top mip exists to protect: crisp 0/255 alpha and nothing in between. If
    // the block path blurred or rounded anything it would show up as partial alpha here and
    // route a fence to the blend pass.
    const int allClear[16] = {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6};
    const int allOpaque[16] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};

    std::vector<uint8_t> blocks;
    Append(blocks, Bc3Block(0, 255, allClear));
    Append(blocks, Bc3Block(0, 255, allOpaque));
    Append(blocks, Bc3Block(0, 255, allOpaque));
    Append(blocks, Bc3Block(0, 255, allOpaque));

    const std::vector<char> paa = BuildDxtPaa(kDxt5Magic, 8, 8, blocks);
    const AlphaStats reference = WholeFileStats(paa);
    REQUIRE(reference.kind == AlphaStats::Cutout);
    REQUIRE(reference.pctPartial == 0.0);

    RequireSameStats(reference, AlphaFromBlocks(blocks.data(), 8, 8, true));
}

TEST_CASE("BC3 alpha that is clear everywhere stays opaque off the blocks", "[graphics][texture]")
{
    // ClassifyAlpha re-routes an all-clear mask to Opaque on purpose (a mask that hides 100%
    // of the surface is packed data, not coverage). Reusing ClassifyAlpha unchanged is what
    // keeps that rule; a hand-rolled counter in the fast path would have lost it.
    const int allClear[16] = {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6};

    std::vector<uint8_t> blocks;
    for (int i = 0; i < 4; i++)
    {
        Append(blocks, Bc3Block(0, 255, allClear));
    }

    const std::vector<char> paa = BuildDxtPaa(kDxt5Magic, 8, 8, blocks);
    const AlphaStats reference = WholeFileStats(paa);
    REQUIRE(reference.aMax == 0);
    REQUIRE(reference.kind == AlphaStats::Opaque);

    RequireSameStats(reference, AlphaFromBlocks(blocks.data(), 8, 8, true));
}

TEST_CASE("BC2 nibble alpha read off the blocks classifies as the full decode does", "[graphics][texture]")
{
    // The expansion is the thing to get wrong here: 0xF must become 255, not 240. A plain
    // `n << 4` shifts every opaque texel to 240, which turns 100% opaque into 100% PARTIAL
    // and sends the whole texture to the blend pass.
    const int ramp[16] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF};
    const int opaque[16] = {0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF};
    const int clear[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    std::vector<uint8_t> blocks;
    Append(blocks, Bc2Block(ramp));
    Append(blocks, Bc2Block(opaque));
    Append(blocks, Bc2Block(clear));
    Append(blocks, Bc2Block(opaque));

    const std::vector<char> paa = BuildDxtPaa(kDxt3Magic, 8, 8, blocks);
    const AlphaStats reference = WholeFileStats(paa);
    REQUIRE(reference.aMax == 255); // the 0xF -> 255 expansion, stated as a value not a hope
    REQUIRE(reference.pctOpaque > 0.0);
    REQUIRE(reference.pctClear > 0.0);
    REQUIRE(reference.pctPartial > 0.0);

    RequireSameStats(reference, AlphaFromBlocks(blocks.data(), 8, 8, false));
}

TEST_CASE("A real PAA classifies the same from one level as from the whole file", "[graphics][texture]")
{
    // Same claim, but end to end over shipped files and the real VFS path: open the texture
    // source the way TextureWgpu::Init opens it, read ONLY level 0, and classify that.
    struct Case
    {
        const char* fixture;
        bool bc3;
    };
    const Case cases[] = {{"texture/paa/synthetic_dxt5.paa", true}, {"texture/paa/synthetic_dxt3.paa", false}};

    for (const Case& c : cases)
    {
        INFO(c.fixture);
        const char* path = GET_FIXTURE(c.fixture);

        // --- reference: the whole file, decoded to RGBA8 ---
        const DecodedImage img = DecodePAAFile(path);
        REQUIRE(img.valid());
        const AlphaStats reference =
            ClassifyAlpha(img.rgba.data(), static_cast<size_t>(img.width) * static_cast<size_t>(img.height));

        // --- block path: one level, alpha only ---
        std::vector<PacLevelMem> mips(MAX_MIPMAPS);
        ITextureSourceFactory* factory = SelectTextureSourceFactory(path);
        REQUIRE(factory != nullptr);
        REQUIRE(factory->Check(path));
        std::unique_ptr<ITextureSource> src(factory->Create(path, mips.data(), MAX_MIPMAPS));
        REQUIRE(src != nullptr);

        const PacFormat fmt = src->GetFormat();
        const int levels = src->GetMipmapCount();
        REQUIRE(levels > 1); // a one-level file would make the "fewer bytes" half vacuous
        for (int i = 0; i < levels; i++)
        {
            mips[i].SetDestFormat(fmt, 8);
        }
        // The block path is only legal because the destination format IS the file format for
        // every DXT level — that is what makes the bytes blocks rather than pixels.
        REQUIRE(mips[0].DstFormat() == fmt);
        REQUIRE(mips[0]._w == img.width);
        REQUIRE(mips[0]._h == img.height);

        const int topSize = render::mipmap::ComputeLayout(fmt, mips[0]._w, mips[0]._h).dataSize;
        REQUIRE(topSize > 0);
        int chainSize = 0;
        for (int i = 0; i < levels; i++)
        {
            chainSize += render::mipmap::ComputeLayout(fmt, mips[i]._w, mips[i]._h).dataSize;
        }
        // The cheap half of the claim: the scan now touches one level, where the whole-file
        // decode read every one of them before deciding it only wanted the first.
        CHECK(topSize < chainSize);

        std::vector<uint8_t> blocks(static_cast<size_t>(topSize));
        const MipmapRead read{blocks.data(), &mips[0], 0};
        const uint64_t opensBefore = TextureMipOpenCount();
        REQUIRE(src->GetMipmapChain(&read, 1));
        CHECK(TextureMipOpenCount() - opensBefore == 1);

        RequireSameStats(reference, AlphaFromBlocks(blocks.data(), mips[0]._w, mips[0]._h, c.bc3));
    }
}

TEST_CASE("Compact block alpha matches every full-decode texel including edge blocks", "[graphics][texture]")
{
    const int indices[16] = {0, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 1, 0};
    const int nibbles[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    for (const bool bc3 : {false, true})
        for (const auto [w, h] : {std::pair{1, 1}, std::pair{2, 3}, std::pair{5, 7},
                                 std::pair{7, 5}, std::pair{8, 8}})
        {
            CAPTURE(bc3, w, h);
            std::vector<uint8_t> blocks;
            const int count = ((w + 3) / 4) * ((h + 3) / 4);
            for (int i = 0; i < count; ++i)
                Append(blocks, bc3 ? Bc3Block(i % 2 ? 17 : 241, i % 2 ? 201 : 7, indices) : Bc2Block(nibbles));
            // PacLevelMem rejects PAA levels smaller than 2 in either dimension.
            // The raw block helper also supports 1x1: decode the same block through
            // a valid 2x2 PAA, then crop the independent RGBA reference to 1x1.
            // Both sizes occupy one block, so the source bytes stay identical.
            const int referenceW = std::max(2, w), referenceH = std::max(2, h);
            const auto paa = BuildDxtPaa(bc3 ? kDxt5Magic : kDxt3Magic, referenceW, referenceH, blocks);
            const auto reference = DecodePAABuffer(paa.data(), paa.size(), true);
            REQUIRE(reference.valid());
            REQUIRE(reference.width == referenceW);
            REQUIRE(reference.height == referenceH);
            std::vector<uint8_t> alpha;
            REQUIRE(DecodeBlockAlpha(blocks.data(), blocks.size(), w, h, bc3, alpha));
            REQUIRE(alpha.size() == static_cast<size_t>(w) * static_cast<size_t>(h));
            std::vector<uint8_t> croppedRgba(alpha.size() * 4);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    const size_t sample = static_cast<size_t>(y) * w + x;
                    const size_t referenceSample = static_cast<size_t>(y) * referenceW + x;
                    CHECK(alpha[sample] == reference.rgba[referenceSample * 4 + 3]);
                    std::copy_n(reference.rgba.data() + referenceSample * 4, 4,
                                croppedRgba.data() + sample * 4);
                }
            RequireSameStats(ClassifyAlpha(croppedRgba.data(), alpha.size()),
                             ClassifyAlphaSamples(alpha.data(), alpha.size(), 1));
        }
}

TEST_CASE("Compact block alpha rejects invalid and truncated inputs", "[graphics][texture]")
{
    const uint8_t block[16] = {};
    std::vector<uint8_t> alpha(64, 123);
    CHECK_FALSE(DecodeBlockAlpha(nullptr, 16, 4, 4, true, alpha));
    CHECK(alpha.empty());
    CHECK_FALSE(DecodeBlockAlpha(block, 16, 0, 4, true, alpha));
    CHECK_FALSE(DecodeBlockAlpha(block, 16, 4, -1, false, alpha));
    CHECK_FALSE(DecodeBlockAlpha(block, 15, 4, 4, true, alpha));
    CHECK_FALSE(DecodeBlockAlpha(block, 16, 5, 4, false, alpha));
    CHECK_FALSE(DecodeBlockAlpha(block, 16, 2147483647, 2147483647, true, alpha));
    CHECK(alpha.empty());
    RequireSameStats(AlphaStats{}, ClassifyAlphaSamples(nullptr, 1, 1));
    RequireSameStats(AlphaStats{}, ClassifyAlphaSamples(block, 0, 1));
    RequireSameStats(AlphaStats{}, ClassifyAlphaSamples(block, 1, 0));
    RequireSameStats(AlphaStats{}, ClassifyAlphaSamples(block, std::numeric_limits<size_t>::max(), 4));
}
