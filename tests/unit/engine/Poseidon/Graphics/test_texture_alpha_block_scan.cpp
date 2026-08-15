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
// Note on what this file can reach: PoseidonTests links Poseidon, not WgpuRenderer, so it
// exercises the primitives TextureWgpu is built from (DecodeBc4Block, GetMipmapChain,
// ClassifyAlpha) rather than the class itself. The invariant is the same one, and it is the
// one that can break underneath the renderer without anybody noticing.
//
// That leaves one real gap: the block walk below is a SECOND authoring of the loop in
// TextureWgpu::ClassifyAlphaFromBlocks, so the shipped copy could drift and every assertion
// here would still pass. `the_renderer_carries_the_same_block_walk` closes it by pinning the
// lines that must not diverge — the same trick far/mod.rs uses to hold its shader and its
// Rust predicate together. A source-text pin is crude, but a test that silently stops
// covering the code it names is worse.

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

using namespace Poseidon;

namespace
{

// ---------------------------------------------------------------------------------------
// The block path, written exactly as TextureWgpu::ClassifyAlphaFromBlocks writes it: fill
// only the alpha lane of an RGBA-shaped buffer, then hand that to the UNMODIFIED
// ClassifyAlpha. Nothing here decodes a colour texel.
AlphaStats AlphaFromBlocks(const uint8_t* blocks, int w, int h, bool bc3)
{
    const int blockCols = (w + 3) / 4;
    const int blockRows = (h + 3) / 4;
    std::vector<uint8_t> alphaOnly(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
    const size_t rowPitch = static_cast<size_t>(w) * 4u;

    for (int by = 0; by < blockRows; by++)
    {
        for (int bx = 0; bx < blockCols; bx++)
        {
            const uint8_t* block = blocks + (static_cast<size_t>(by) * blockCols + bx) * 16u;
            uint8_t* dst =
                alphaOnly.data() + static_cast<size_t>(by) * 4u * rowPitch + static_cast<size_t>(bx) * 16u + 3u;
            const int usableW = std::min(4, w - bx * 4);
            const int usableH = std::min(4, h - by * 4);
            if (bc3)
            {
                DecodeBc4Block(block, dst, rowPitch, 4, usableW, usableH);
            }
            else
            {
                for (int i = 0; i < 16; i++)
                {
                    const int py = i / 4;
                    const int px = i % 4;
                    if (py >= usableH || px >= usableW)
                    {
                        continue;
                    }
                    const int nibble = (i & 1) ? (block[i / 2] >> 4) : (block[i / 2] & 0xF);
                    dst[static_cast<size_t>(py) * rowPitch + static_cast<size_t>(px) * 4u] =
                        static_cast<uint8_t>((nibble << 4) | nibble);
                }
            }
        }
    }
    return ClassifyAlpha(alphaOnly.data(), static_cast<size_t>(w) * static_cast<size_t>(h));
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

// The block walk above is a second authoring of TextureWgpu::ClassifyAlphaFromBlocks. Nothing
// else stops the shipped copy drifting — a wrong nibble order or a dropped edge clamp there
// would leave every assertion in this file passing while the renderer misclassified every
// BC2 cutout in the game.
//
// Pinned as source text because PoseidonTests cannot link WgpuRenderer to call the real
// thing. If this fails because the renderer was legitimately rewritten, re-derive the walk
// above against it rather than deleting the check.
TEST_CASE("The renderer carries the same block walk", "[graphics][texture]")
{
    // Walked rather than counted: a fixed number of parent_path() calls is silently wrong the
    // day this file moves a directory, and it was wrong by one when first written.
    std::filesystem::path src;
    for (std::filesystem::path at = std::filesystem::path(__FILE__).parent_path(); !at.empty();
         at = at.parent_path())
    {
        const std::filesystem::path candidate = at / "engine" / "WgpuRenderer" / "TextureWgpu.cpp";
        if (std::filesystem::is_regular_file(candidate))
        {
            src = candidate;
            break;
        }
        if (at == at.root_path())
        {
            break;
        }
    }
    INFO("searched upward from " << std::filesystem::path(__FILE__).parent_path().string()
                                 << " for engine/WgpuRenderer/TextureWgpu.cpp");
    REQUIRE(std::filesystem::is_regular_file(src));

    std::ifstream in(src, std::ios::binary);
    REQUIRE(in.good());
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // BC3's alpha half IS a BC4 block — the whole reason this shortcut is exact.
    CHECK(text.find("DecodeBc4Block(block, dst, rowPitch, 4, usableW, usableH);") != std::string::npos);
    // BC2's low-nibble-first expansion. `n << 4` alone turns 100% opaque into 100% partial.
    CHECK(text.find("(block[i / 2] >> 4) : (block[i / 2] & 0xF)") != std::string::npos);
    CHECK(text.find("(nibble << 4) | nibble") != std::string::npos);
    // The alpha lane is byte 3, and the verdict still comes from the unmodified classifier.
    CHECK(text.find("ClassifyAlpha(alphaOnly.data()") != std::string::npos);
    // The edge clamp, which is dormant on power-of-two PAA levels and would go unnoticed.
    CHECK(text.find("std::min(4, w - bx * 4)") != std::string::npos);
    CHECK(text.find("std::min(4, h - by * 4)") != std::string::npos);
}
