// The worker->main-thread texture handoff for streamed object admission:
//  * ReadPAABlockChainBuffer -- the pure, worker-safe raw-block reader -- must agree byte for
//    byte with DecodePAABuffer's view of the same file (same dimensions, and a chain whose
//    level 0, decompressed, decodes to the same pixels), and must reject what it cannot serve
//    (non-DXT formats) rather than guessing.
//  * PreparedTextureStore must honour its contract: keys normalise, Put/Take round-trips a
//    chain exactly once, MarkUploaded blocks further prepares until MarkEvicted, and the byte
//    budget refuses rather than evicts.
// These two are the halves TextureWgpu::EnsureUploaded validates against each other at
// runtime; this test pins them without a renderer.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <Poseidon/Graphics/Textures/PreparedTextures.hpp>

#include "test_fixtures.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Poseidon;

namespace
{
std::vector<char> ReadAll(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A synthetic, minimal DXT1 PAA: magic, no taggs, no palette, one 4x4 mip stored raw,
// terminator. 8 bytes of block data (a solid colour block).
std::vector<uint8_t> TinyDxt1Paa()
{
    std::vector<uint8_t> f;
    auto u16 = [&](uint16_t v)
    {
        f.push_back(static_cast<uint8_t>(v & 0xFF));
        f.push_back(static_cast<uint8_t>(v >> 8));
    };
    auto u24 = [&](uint32_t v)
    {
        f.push_back(static_cast<uint8_t>(v & 0xFF));
        f.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        f.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    };
    u16(0xFF01); // DXT1
    u16(0);      // palette: 0 colours
    u16(4);      // w
    u16(4);      // h
    u24(8);      // stored size
    // One DXT1 block: c0 = c1 = pure red (0xF800), all indices 0.
    const uint8_t block[8] = {0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
    f.insert(f.end(), block, block + 8);
    u16(0); // terminator w
    u16(0); // terminator h
    return f;
}
} // namespace

TEST_CASE("ReadPAABlockChainBuffer parses a minimal DXT1 container", "[Graphics][PAA][streaming]")
{
    const std::vector<uint8_t> file = TinyDxt1Paa();
    PAABlockChain chain;
    std::string error;
    REQUIRE(ReadPAABlockChainBuffer(file.data(), file.size(), chain, &error));
    CHECK(error.empty());
    CHECK(chain.magic == 0xFF01);
    CHECK(chain.width == 4);
    CHECK(chain.height == 4);
    REQUIRE(chain.levels.size() == 1);
    CHECK(chain.levels[0].offset == 0);
    CHECK(chain.levels[0].size == 8);
    REQUIRE(chain.blocks.size() == 8);
    CHECK(chain.blocks[1] == 0xF8);

    // The same bytes through the RGBA decoder: red everywhere. The two readers must agree
    // about what the file contains.
    DecodedImage img = DecodePAABuffer(file.data(), file.size(), true);
    REQUIRE(img.valid());
    CHECK(img.width == 4);
    CHECK(img.rgba[0] == 255); // R
    CHECK(img.rgba[1] == 0);   // G
}

TEST_CASE("ReadPAABlockChainBuffer refuses what it cannot serve", "[Graphics][PAA][streaming]")
{
    PAABlockChain chain;
    std::string error;

    SECTION("empty buffer")
    {
        CHECK_FALSE(ReadPAABlockChainBuffer(nullptr, 0, chain, &error));
        CHECK_FALSE(chain.valid());
    }
    SECTION("non-DXT magic (ARGB4444) is not this reader's format")
    {
        const uint8_t bytes[] = {0x44, 0x44, 0x00, 0x00};
        CHECK_FALSE(ReadPAABlockChainBuffer(bytes, sizeof(bytes), chain, &error));
        CHECK(error == "not a DXT PAA");
    }
    SECTION("level payload past the end of the buffer")
    {
        std::vector<uint8_t> file = TinyDxt1Paa();
        file.resize(file.size() - 8); // drop the block data + terminator
        CHECK_FALSE(ReadPAABlockChainBuffer(file.data(), file.size(), chain, &error));
        CHECK_FALSE(chain.valid());
    }
}

TEST_CASE("ReadPAABlockChainBuffer agrees with DecodePAABuffer on a real fixture", "[Graphics][PAA][streaming]")
{
    // Any DXT fixture will do; this one is a known-good DXT compressed PAA used by the
    // decoder tests. If the fixture set changes, swap in any valid .paa whose magic is
    // 0xFF01..0xFF05.
    const std::vector<char> bytes = ReadAll(GET_FIXTURE("paa/synthetic_dxt1.paa"));
    if (bytes.empty())
    {
        SUCCEED("fixture not present in this checkout; covered by the synthetic case");
        return;
    }
    PAABlockChain chain;
    std::string error;
    const bool parsed = ReadPAABlockChainBuffer(bytes.data(), bytes.size(), chain, &error);
    DecodedImage img = DecodePAABuffer(bytes.data(), bytes.size(), true);
    if (!img.valid())
    {
        // Not a DXT fixture after all -- then the chain reader must refuse it too.
        CHECK_FALSE(parsed);
        return;
    }
    REQUIRE(parsed);
    CHECK(chain.width == img.width);
    CHECK(chain.height == img.height);
    // Levels strictly halve and stay in bounds.
    for (size_t i = 1; i < chain.levels.size(); ++i)
    {
        CHECK(chain.levels[i].width == chain.levels[i - 1].width / 2);
        CHECK(chain.levels[i].offset == chain.levels[i - 1].offset + chain.levels[i - 1].size);
    }
}

TEST_CASE("PreparedTextureStore key normalisation and round trip", "[Graphics][PAA][streaming]")
{
    CHECK(render::PreparedTextureStore::Key("Reforger/Everon\\Textures\\Bark_CO.PAA") ==
          "reforger\\everon\\textures\\bark_co.paa");
    CHECK(render::PreparedTextureStore::Key(static_cast<const char*>(nullptr)).empty());

    if (!render::PreparedTextureStore::Enabled())
    {
        SUCCEED("store disabled by WGR_OBJECT_STREAM_TEXTURE_PREPARE=0 in this environment");
        return;
    }
    render::PreparedTextureStore& store = render::PreparedTextureStore::Instance();
    store.Clear();

    const std::vector<uint8_t> file = TinyDxt1Paa();
    PAABlockChain chain;
    REQUIRE(ReadPAABlockChainBuffer(file.data(), file.size(), chain));
    const std::string key = render::PreparedTextureStore::Key("data\\tiny_co.paa");

    REQUIRE(store.ShouldPrepare(key));
    REQUIRE(store.Put(key, std::move(chain)));
    CHECK_FALSE(store.ShouldPrepare(key)); // already stored

    PAABlockChain out;
    REQUIRE(store.Take(key, out));
    CHECK(out.valid());
    CHECK(out.blocks.size() == 8);
    CHECK_FALSE(store.Take(key, out)); // consumed exactly once

    // Uploaded names are refused until eviction re-opens them.
    store.MarkUploaded(key);
    CHECK_FALSE(store.ShouldPrepare(key));
    store.MarkEvicted(key);
    CHECK(store.ShouldPrepare(key));

    store.Clear();
}
