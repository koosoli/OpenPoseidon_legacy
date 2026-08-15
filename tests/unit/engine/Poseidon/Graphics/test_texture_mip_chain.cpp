// The batched mipmap read (ITextureSource::GetMipmapChain).
//
// A texture upload wants the whole mip chain, and TextureSourcePac used to answer that one
// level at a time — re-opening the texture through the VFS for every level. That is per-CALL
// cost, not per-BYTE cost: the bytes have to be LZO-decompressed either way, but the opens do
// not, and there are as many of them as the texture has levels.
//
// Two things are pinned here, because only the pair is worth anything:
//   - the chain read costs exactly ONE open regardless of level count, and
//   - it produces byte-for-byte what the per-level reads produced.
// Without the second, "fewer opens" would be satisfied by a read that returns garbage.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Graphics/Core/MipmapLayout.hpp>
#include <Poseidon/Graphics/Rendering/Font/Pactext.hpp>
#include <Poseidon/Graphics/Textures/TextureBank.hpp> // MAX_MIPMAPS

#include "../test_fixtures.hpp"

#include <cstdint>
#include <memory>
#include <vector>

using namespace Poseidon;

namespace
{

// MAX_MIPMAPS levels of layout, opened the way TextureWgpu::Init opens one: pick the factory
// by extension, Create() it over a mip array, then fix each level's destination format.
struct OpenedTexture
{
    std::unique_ptr<ITextureSource> src;
    std::vector<PacLevelMem> mips;
    int levels = 0;
    PacFormat dst = PacFormatN;

    std::vector<size_t> offsets;
    size_t total = 0;
};

OpenedTexture OpenFixture(const char* fixture)
{
    OpenedTexture out;
    out.mips.resize(MAX_MIPMAPS);

    ITextureSourceFactory* factory = SelectTextureSourceFactory(fixture);
    REQUIRE(factory != nullptr);
    REQUIRE(factory->Check(fixture));

    out.src.reset(factory->Create(fixture, out.mips.data(), MAX_MIPMAPS));
    REQUIRE(out.src != nullptr);

    out.dst = out.src->GetFormat();
    out.levels = out.src->GetMipmapCount();
    for (int i = 0; i < out.levels; i++)
    {
        out.mips[i].SetDestFormat(out.dst, 8);
    }

    out.offsets.resize(static_cast<size_t>(out.levels));
    for (int i = 0; i < out.levels; i++)
    {
        out.offsets[i] = out.total;
        out.total += static_cast<size_t>(render::mipmap::ComputeLayout(out.dst, out.mips[i]._w, out.mips[i]._h).dataSize);
    }
    return out;
}

// The fixture is a real 64-square DXT5 PAA, so it carries a genuine multi-level chain rather
// than a hand-built one — the point of the test is the shape of the file access, and a
// synthesised single-level source could not fail the assertion below.
constexpr const char* kFixture = "texture/paa/synthetic_dxt5.paa";

} // namespace

TEST_CASE("A batched mip chain read opens the texture once", "[graphics][texture]")
{
    OpenedTexture tex = OpenFixture(GET_FIXTURE(kFixture));
    // One level would make both halves of this test vacuously true.
    REQUIRE(tex.levels > 1);
    REQUIRE(tex.total > 0);

    // --- level at a time, the way every caller read a chain before GetMipmapChain ---
    std::vector<uint8_t> perLevel(tex.total);
    const uint64_t opensBeforeLoop = TextureMipOpenCount();
    for (int i = 0; i < tex.levels; i++)
    {
        REQUIRE(tex.src->GetMipmapData(perLevel.data() + tex.offsets[i], tex.mips[i], i));
    }
    const uint64_t loopOpens = TextureMipOpenCount() - opensBeforeLoop;

    // --- the same chain in one call ---
    std::vector<uint8_t> batched(tex.total);
    std::vector<MipmapRead> reads(static_cast<size_t>(tex.levels));
    for (int i = 0; i < tex.levels; i++)
    {
        reads[i] = MipmapRead{batched.data() + tex.offsets[i], &tex.mips[i], i};
    }
    const uint64_t opensBeforeChain = TextureMipOpenCount();
    REQUIRE(tex.src->GetMipmapChain(reads.data(), tex.levels));
    const uint64_t chainOpens = TextureMipOpenCount() - opensBeforeChain;

    // The saving IS the level count: one open per level becomes one open, full stop.
    CHECK(loopOpens == static_cast<uint64_t>(tex.levels));
    CHECK(chainOpens == 1);

    // ...and it is the same texture. Compared over the whole packed chain, so a level read
    // at the wrong seek offset shows up here rather than as a corrupt mip in the game.
    CHECK(batched == perLevel);
}

TEST_CASE("A batched mip chain read rejects a level it cannot serve", "[graphics][texture]")
{
    OpenedTexture tex = OpenFixture(GET_FIXTURE(kFixture));
    REQUIRE(tex.levels > 0);

    std::vector<uint8_t> blocks(tex.total);
    std::vector<MipmapRead> reads(static_cast<size_t>(tex.levels));
    for (int i = 0; i < tex.levels; i++)
    {
        reads[i] = MipmapRead{blocks.data() + tex.offsets[i], &tex.mips[i], i};
    }
    // A null layout stands in for any level the source cannot answer. The contract that
    // matters to the caller is that a partial chain is reported as a failure — TextureWgpu
    // falls back to a whole-file decode on false, and would upload undefined bytes on true.
    reads[tex.levels - 1].mip = nullptr;
    CHECK_FALSE(tex.src->GetMipmapChain(reads.data(), tex.levels));

    // An empty request is not a failure; it is nothing to do.
    CHECK(tex.src->GetMipmapChain(nullptr, 0));
    CHECK(tex.src->GetMipmapChain(reads.data(), 0));
}
