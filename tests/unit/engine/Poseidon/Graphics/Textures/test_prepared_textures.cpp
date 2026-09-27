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
#include <Poseidon/IO/Streams/ArchiveSourceBinding.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>

#include "test_fixtures.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include <thread>

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
    CHECK(chain.levels[0].sourceHeaderOffset == 4);
    CHECK(chain.levels[0].legacyBlockReadCompatible);
    const PAABlockLevel synthetic;
    CHECK(synthetic.sourceHeaderOffset == SIZE_MAX);
    CHECK_FALSE(synthetic.legacyBlockReadCompatible);
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

TEST_CASE("Prepared texture probes preserve payloads of the other kind", "[Graphics][PAA][streaming]")
{
    auto& store = render::PreparedTextureStore::Instance();
    if (!store.Enabled())
    {
        SUCCEED("preparation disabled by environment");
        return;
    }
    store.Clear();
    const auto bytes = TinyDxt1Paa();
    DecodedImage image = DecodePAABuffer(bytes.data(), bytes.size(), true);
    REQUIRE(image.valid());
    const auto expected = image.rgba;
    const auto imageCapacity = image.rgba.capacity();
    REQUIRE(store.PutDecoded("typed-image", std::move(image)));
    PAABlockChain chain;
    CHECK_FALSE(store.Take("typed-image", chain));
    CHECK_FALSE(chain.valid());
    CHECK(store.SnapshotStats().bytes == imageCapacity);
    REQUIRE(store.TakeDecoded("typed-image", image));
    CHECK(image.rgba == expected);
    CHECK(store.SnapshotStats().bytes == 0);

    REQUIRE(ReadPAABlockChainBuffer(bytes.data(), bytes.size(), chain));
    const auto blocks = chain.blocks;
    const auto chainCapacity = chain.blocks.capacity() + chain.levels.capacity() * sizeof(PAABlockLevel);
    REQUIRE(store.Put("typed-chain", std::move(chain)));
    CHECK_FALSE(store.TakeDecoded("typed-chain", image));
    CHECK(store.SnapshotStats().bytes == chainCapacity);
    REQUIRE(store.Take("typed-chain", chain));
    CHECK(chain.blocks == blocks);
    CHECK(store.SnapshotStats().bytes == 0);
    store.Clear();
}

TEST_CASE("Uploaded decoded textures release their budget and block reprepare", "[Graphics][PAA][streaming]")
{
    auto& store = render::PreparedTextureStore::Instance();
    if (!store.Enabled())
    {
        SUCCEED("preparation disabled by environment");
        return;
    }
    store.Clear();
    const auto bytes = TinyDxt1Paa();
    auto image = DecodePAABuffer(bytes.data(), bytes.size(), true);
    REQUIRE(store.PutDecoded("uploaded-image", std::move(image)));
    store.MarkUploaded("uploaded-image");
    CHECK(store.SnapshotStats().bytes == 0);
    CHECK(store.SnapshotStats().entries == 0);
    CHECK_FALSE(store.TakeDecoded("uploaded-image", image));
    CHECK_FALSE(store.ShouldPrepare("uploaded-image"));
    store.MarkEvicted("uploaded-image");
    CHECK(store.ShouldPrepare("uploaded-image"));
    store.Clear();
}

TEST_CASE("PreparedTextureStore rejects publication from a cleared world", "[Graphics][PAA][streaming][cancellation]")
{
    if (!render::PreparedTextureStore::Enabled())
        SKIP("Requires texture preparation");
    auto& store = render::PreparedTextureStore::Instance();
    store.Clear();
    const uint64_t oldGeneration = store.Generation();
    const uint64_t rejectedBefore = store.SnapshotStats().rejectedStale;
    const auto bytes = TinyDxt1Paa();
    PAABlockChain chain;
    REQUIRE(ReadPAABlockChainBuffer(bytes.data(), bytes.size(), chain));
    DecodedImage image = DecodePAABuffer(bytes.data(), bytes.size(), true);
    REQUIRE(image.valid());
    // A worker has already read/decoded here, but has not published yet.
    store.Clear();
    const uint64_t currentGeneration = store.Generation();
    CHECK(currentGeneration != oldGeneration);
    CHECK_FALSE(store.Put("stale-chain", std::move(chain), oldGeneration));
    CHECK_FALSE(store.PutDecoded("stale-image", std::move(image), oldGeneration));
    CHECK(store.SnapshotStats().entries == 0);
    CHECK(store.SnapshotStats().bytes == 0);
    CHECK(store.SnapshotStats().rejectedStale == rejectedBefore + 2);

    REQUIRE(ReadPAABlockChainBuffer(bytes.data(), bytes.size(), chain));
    REQUIRE(store.Put("same-name", std::move(chain), currentGeneration));
    image = DecodePAABuffer(bytes.data(), bytes.size(), true);
    CHECK_FALSE(store.PutDecoded("same-name", std::move(image), oldGeneration));
    PAABlockChain out;
    REQUIRE(store.Take("same-name", out));
    CHECK(out.valid()); // An obsolete result cannot replace or erase a current entry.
    image = DecodePAABuffer(bytes.data(), bytes.size(), true);
    REQUIRE(store.PutDecoded("current-image", std::move(image), currentGeneration));
    REQUIRE(store.TakeDecoded("current-image", image));
    CHECK(image.valid());
    store.Clear();
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

#include <Poseidon/Graphics/Textures/MipDemand.hpp>

TEST_CASE("Adaptive mip demand is conservative across stale feedback and view changes", "[streaming][mip-demand]")
{
    Poseidon::render::MipDemand demand;
    CHECK(demand.Choose(2, 255, 0, 32, 100, false, 3) == 0);
    CHECK(demand.Choose(2, 4, 90, 32, 100, false, 3) == 0);
    CHECK(demand.Choose(2, 4, 0, 0, 100, false, 3) == 0);
    CHECK(demand.Choose(2, 4, 0, 32, 100, true, 3) == 0);
    CHECK(demand.Choose(2, 0, 0, 32, 100, false, 3) == 0);
}
TEST_CASE("Adaptive mip demand demotes slowly and converts relative feedback to source mips", "[streaming][mip-demand]")
{
    Poseidon::render::MipDemand demand;
    CHECK(demand.Choose(0, 3, 0, 32, 100, false, 3) == 0);
    CHECK(demand.Choose(0, 3, 0, 32, 219, false, 3) == 0);
    CHECK(demand.Choose(0, 3, 0, 32, 220, false, 3) == 2);
    demand.Replaced(220);
    CHECK(demand.Choose(2, 255, 0, 32, 224, false, 3) == 2);
    CHECK(demand.Choose(2, 1, 0, 32, 230, false, 3) == 2);
    CHECK(demand.Choose(2, 0, 0, 32, 231, false, 3) == 0);
    demand.Replaced(240);
    CHECK(demand.Choose(2, 255, 0, 32, 400, false, 3) == 0);
}


TEST_CASE("PAA worker bound rejects before block allocation and sums all levels", "[Graphics][PAA][streaming-budget]")
{
    auto file = TinyDxt1Paa();
    PAABlockChain chain;
    std::string error;
    REQUIRE(ReadPAABlockChainBuffer(file.data(), file.size(), chain, &error, 8));
    CHECK_FALSE(ReadPAABlockChainBuffer(file.data(), file.size(), chain, &error, 7));
    CHECK_FALSE(chain.valid());
    // Duplicate the level before the terminator; each level fits but the total does not.
    const auto first = file;
    file.insert(file.end()-4, first.begin()+4, first.end()-4);
    CHECK_FALSE(ReadPAABlockChainBuffer(file.data(), file.size(), chain, &error, 8));
    REQUIRE(ReadPAABlockChainBuffer(file.data(), file.size(), chain, &error, 16));
    CHECK(chain.blocks.size() == 16);
    // A small claimed compressed input must not trigger a large output allocation.
    file = first;
    file[4] = 0; file[5] = 0xA0; // LZO flag + width 8192
    file[6] = 0; file[7] = 0x20; // height 8192
    CHECK_FALSE(ReadPAABlockChainBuffer(file.data(), file.size(), chain, &error, 1024));
    CHECK(error == "block chain exceeds preparation byte limit");
    CHECK_FALSE(chain.valid());
}

TEST_CASE("Prepared PAA budgets charge retained capacity and release it exactly", "[Graphics][PAA][streaming-budget]")
{
    auto& store = render::PreparedTextureStore::Instance();
    if (!store.Enabled()) SKIP("Requires texture preparation");
    store.Clear();
    const auto bytes = TinyDxt1Paa();
    PAABlockChain chain;
    REQUIRE(ReadPAABlockChainBuffer(bytes.data(), bytes.size(), chain));
    chain.blocks.reserve(4096);
    chain.levels.reserve(32);
    const auto retained = chain.blocks.capacity() + chain.levels.capacity() * sizeof(PAABlockLevel);
    REQUIRE(store.Put("capacity-chain", std::move(chain)));
    CHECK(store.SnapshotStats().bytes == retained);
    REQUIRE(store.Take("capacity-chain", chain));
    CHECK(store.SnapshotStats().bytes == 0);
    CHECK(chain.blocks.capacity() + chain.levels.capacity() * sizeof(PAABlockLevel) == retained);
    auto image = DecodePAABuffer(bytes.data(), bytes.size(), true);
    image.rgba.reserve(8192);
    const auto imageCapacity = image.rgba.capacity();
    REQUIRE(store.PutDecoded("capacity-image", std::move(image)));
    CHECK(store.SnapshotStats().bytes == imageCapacity);
    store.MarkUploaded("capacity-image");
    CHECK(store.SnapshotStats().bytes == 0);
    store.Clear();
}

TEST_CASE("Prepared PAA refuses tiny payload with oversized retained capacity", "[Graphics][PAA][streaming-budget]")
{
    auto& store = render::PreparedTextureStore::Instance();
    if (!store.Enabled() || store.ByteBudget() > 4 * 1024 * 1024)
        SKIP("Run with WGR_OBJECT_STREAM_TEXTURE_PREPARE_MB=1 to bound fixture allocation");
    store.Clear();
    const auto bytes = TinyDxt1Paa();
    PAABlockChain chain;
    REQUIRE(ReadPAABlockChainBuffer(bytes.data(), bytes.size(), chain));
    chain.blocks.reserve(store.ByteBudget() + 1);
    CHECK_FALSE(store.Put("oversized-capacity-chain", std::move(chain)));
    CHECK(store.SnapshotStats().bytes == 0);
    auto image = DecodePAABuffer(bytes.data(), bytes.size(), true);
    image.rgba.reserve(store.ByteBudget() + 1);
    CHECK_FALSE(store.PutDecoded("oversized-capacity-image", std::move(image)));
    CHECK(store.SnapshotStats().bytes == 0);
    CHECK(store.SnapshotStats().entries == 0);
    store.Clear();
}

#ifdef _WIN32
namespace
{
struct AlphaArchiveFixture
{
    std::filesystem::path path;
    QFBank bank;
    BankReadRequest read;
    PAABlockChain chain;
    std::vector<uint8_t> file;
    AlphaArchiveFixture(uint16_t magic = 0xFF03, int layout = 0)
    {
        file = {static_cast<uint8_t>(magic), static_cast<uint8_t>(magic >> 8), 0, 0, 4, 0, 4, 0, 16, 0, 0};
        // BC2: alternating clear/opaque; BC3: endpoint/index mixtures.
        const uint8_t block[16] = {0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0,
                                   0, 0xF8, 0, 0xF8, 0, 0, 0, 0};
        file.insert(file.end(), block, block + 16);
        file.insert(file.end(), 4, 0);
        if (layout == 1)
        {
            // Original harmless unknown TAGG changes raw mip-header position,
            // while magic, dimensions and decoded top span remain identical.
            const uint8_t tag[] = {'G', 'G', 'A', 'T', 'T', 'S', 'E', 'T', 1, 0, 0, 0, 42};
            file.insert(file.begin() + 2, tag, tag + sizeof(tag));
        }
        if (layout == 2)
        {
            file[4] = 0xD2; file[5] = 0x04; // legacy marker 1234,8765
            file[6] = 0x3D; file[7] = 0x22;
            const uint8_t dimensions[] = {4, 0, 4, 0};
            file.insert(file.begin() + 8, dimensions, dimensions + 4);
        }
        if (layout == 3)
        {
            file[8] = 17; // parser still accepts extra raw bytes; facts must refuse
            file.insert(file.end() - 4, 77);
        }
        path = std::filesystem::temp_directory_path() / ("prepared_alpha_" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".pbo");
        std::ofstream output(path, std::ios::binary);
        output.write("member.paa", 11);
        auto u32 = [&](uint32_t v) {
            const char bytes[4] = {static_cast<char>(v), static_cast<char>(v >> 8),
                                  static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
            output.write(bytes, 4);
        };
        u32(0); u32(static_cast<uint32_t>(file.size())); u32(0); u32(0); u32(static_cast<uint32_t>(file.size()));
        output.put(0); for (int i = 0; i < 5; ++i) u32(0);
        output.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
        REQUIRE(output.good()); output.close();
        auto name = path; name.replace_extension();
        REQUIRE(bank.open(RString(name.string().c_str()))); bank.Lock();
        auto request = bank.CaptureReadRequest("member.paa", true);
        REQUIRE(request); read = *request;
        std::vector<char> owned;
        REQUIRE(read.Read(owned));
        REQUIRE(ReadPAABlockChainBuffer(owned.data(), owned.size(), chain, nullptr, 16 * 1024 * 1024));
    }
    ~AlphaArchiveFixture()
    {
        bank.close(); read.archiveLease.reset();
        std::error_code error; std::filesystem::remove(path, error);
    }
};
void CheckAlphaFields(const AlphaStats& actual, const AlphaStats& expected)
{
    CHECK(actual.kind == expected.kind); CHECK(actual.aMin == expected.aMin);
    CHECK(actual.aMax == expected.aMax); CHECK(actual.aMean == expected.aMean);
    CHECK(actual.pctClear == expected.pctClear); CHECK(actual.pctOpaque == expected.pctOpaque);
    CHECK(actual.pctPartial == expected.pctPartial); CHECK(actual.pctMid == expected.pctMid);
}
}

TEST_CASE("Leased prepared alpha facts match source pixels and retain the upload chain", "[Graphics][PAA][streaming][alpha-facts]")
{
    auto& store = render::PreparedTextureStore::Instance();
    if (!store.Enabled()) SKIP("Requires texture preparation");
    store.Clear();
    for (const uint16_t magic : {uint16_t(0xFF02), uint16_t(0xFF03), uint16_t(0xFF04), uint16_t(0xFF05)})
    {
        AlphaArchiveFixture fixture(magic);
        const auto image = DecodePAABuffer(fixture.file.data(), fixture.file.size(), true);
        REQUIRE(image.valid());
        const auto expected = ClassifyAlpha(image.rgba.data(), 16);
        auto facts = store.BuildAlphaFacts(fixture.chain, fixture.read);
        REQUIRE(facts); CheckAlphaFields(facts->histogram, expected);
        if ((expected.kind == AlphaStats::Cutout && expected.pctClear > 0) ||
            (expected.kind == AlphaStats::Blend && expected.pctClear >= 2))
        {
            const auto shape = MeasureAlphaShapeAnalysis(image.rgba.data(), 4, 4, 4, 3);
            CHECK(facts->shape.measured == shape.measured);
            CHECK(facts->shape.meanClearNeighbours == shape.meanClearNeighbours);
            CHECK(facts->shape.clusteredClearFrac == shape.clusteredClearFrac);
            CHECK(facts->shape.partialBandWidth == shape.partialBandWidth);
        }
        const auto chainCharge = fixture.chain.blocks.capacity() + fixture.chain.levels.capacity() * sizeof(PAABlockLevel);
        const auto factCharge = sizeof(render::PreparedAlphaFacts) + facts->source.archive.capacity() + 1 +
            facts->source.ArchiveIdentityBytes() + 64;
        const auto blocks = fixture.chain.blocks;
        REQUIRE(store.Put("alpha-member", std::move(fixture.chain), store.Generation(), std::move(facts)));
        CHECK(store.SnapshotStats().bytes == chainCharge + factCharge);
        CHECK(store.HasAlphaFacts("alpha-member"));
        render::PreparedAlphaFacts copied;
        REQUIRE(store.CopyAlphaFacts("alpha-member", fixture.read, magic, 4, 4, 16, 4, copied));
        CheckAlphaFields(copied.histogram, expected);
        CHECK(store.SnapshotStats().bytes == chainCharge + factCharge);
        PAABlockChain upload;
        REQUIRE(store.Take("alpha-member", upload));
        CHECK(upload.blocks == blocks); CHECK(upload.magic == magic);
        CHECK_FALSE(store.HasAlphaFacts("alpha-member"));
        CHECK(store.SnapshotStats().bytes == 0);
        store.Clear();
    }
}

TEST_CASE("Prepared alpha facts refuse mismatch cancellation and departed generations", "[Graphics][PAA][streaming][alpha-facts]")
{
    auto& store = render::PreparedTextureStore::Instance();
    if (!store.Enabled()) SKIP("Requires texture preparation");
    store.Clear();
    AlphaArchiveFixture fixture;
    const auto generation = store.Generation();
    SECTION("phase cancellation never returns partial facts")
    {
        for (int stopAt = 1; stopAt <= 4; ++stopAt)
        {
            int calls = 0;
            CHECK_FALSE(store.BuildAlphaFacts(fixture.chain, fixture.read, [&] { return ++calls == stopAt; }));
            CHECK(calls == stopAt);
        }
    }
    SECTION("unsupported or truncated chain preserves ordinary fallback")
    {
        auto unleased = fixture.read; unleased.archiveLease.reset();
        CHECK_FALSE(store.BuildAlphaFacts(fixture.chain, unleased));
        fixture.chain.magic = 0xFF01;
        CHECK_FALSE(store.BuildAlphaFacts(fixture.chain, fixture.read));
        fixture.chain.magic = 0xFF03; fixture.chain.levels[0].size = 15;
        CHECK_FALSE(store.BuildAlphaFacts(fixture.chain, fixture.read));
    }
    SECTION("missing provenance is unknown even for otherwise identical blocks")
    {
        fixture.chain.levels[0].sourceHeaderOffset = SIZE_MAX;
        CHECK_FALSE(store.BuildAlphaFacts(fixture.chain, fixture.read));
        fixture.chain.levels[0].sourceHeaderOffset = 4;
        fixture.chain.levels[0].legacyBlockReadCompatible = false;
        CHECK_FALSE(store.BuildAlphaFacts(fixture.chain, fixture.read));
    }
    SECTION("TAGG layout binds the stored source cursor rather than decoded block offset")
    {
        AlphaArchiveFixture tagged(0xFF03, 1);
        REQUIRE(tagged.chain.levels[0].sourceHeaderOffset == 17);
        REQUIRE(tagged.chain.levels[0].offset == 0);
        REQUIRE(tagged.chain.levels[0].legacyBlockReadCompatible);
        CHECK(tagged.chain.blocks == fixture.chain.blocks);
        auto facts = store.BuildAlphaFacts(tagged.chain, tagged.read); REQUIRE(facts);
        REQUIRE(facts->sourceHeaderOffset == 17);
        REQUIRE(store.Put("tagged-member", std::move(tagged.chain), generation, std::move(facts)));
        render::PreparedAlphaFacts copied;
        CHECK_FALSE(store.CopyAlphaFacts("tagged-member", tagged.read, 0xFF03, 4, 4, 16, 4, copied));
        REQUIRE(store.CopyAlphaFacts("tagged-member", tagged.read, 0xFF03, 4, 4, 16, 17, copied));
    }
    SECTION("legacy-incompatible accepted source layouts still retain ordinary upload bytes")
    {
        for (int layout : {2, 3})
        {
            AlphaArchiveFixture unsupported(0xFF03, layout);
            REQUIRE(unsupported.chain.valid());
            CHECK(unsupported.chain.blocks == fixture.chain.blocks);
            CHECK_FALSE(unsupported.chain.levels[0].legacyBlockReadCompatible);
            CHECK_FALSE(store.BuildAlphaFacts(unsupported.chain, unsupported.read));
            REQUIRE(store.Put("unsupported-member", std::move(unsupported.chain), generation));
            CHECK_FALSE(store.HasAlphaFacts("unsupported-member"));
            PAABlockChain upload; REQUIRE(store.Take("unsupported-member", upload));
            CHECK(upload.blocks == fixture.chain.blocks);
        }
    }
    SECTION("different chain storage cannot borrow facts")
    {
        auto facts = store.BuildAlphaFacts(fixture.chain, fixture.read); REQUIRE(facts);
        auto other = fixture.chain;
        REQUIRE(store.Put("alpha-member", std::move(other), generation, std::move(facts)));
        CHECK_FALSE(store.HasAlphaFacts("alpha-member"));
        PAABlockChain upload; REQUIRE(store.Take("alpha-member", upload));
        CHECK(upload.blocks == fixture.chain.blocks);
    }
    SECTION("claim checks physical source format dimensions and top span")
    {
        auto facts = store.BuildAlphaFacts(fixture.chain, fixture.read); REQUIRE(facts);
        REQUIRE(store.Put("alpha-member", std::move(fixture.chain), generation, std::move(facts)));
        AlphaArchiveFixture other;
        render::PreparedAlphaFacts copied;
        CHECK_FALSE(store.CopyAlphaFacts("alpha-member", other.read, 0xFF03, 4, 4, 16, 4, copied));
        CHECK_FALSE(store.CopyAlphaFacts("alpha-member", fixture.read, 0xFF05, 4, 4, 16, 4, copied));
        CHECK_FALSE(store.CopyAlphaFacts("alpha-member", fixture.read, 0xFF03, 8, 4, 16, 4, copied));
        CHECK_FALSE(store.CopyAlphaFacts("alpha-member", fixture.read, 0xFF03, 4, 4, 15, 4, copied));
        CHECK_FALSE(store.CopyAlphaFacts("alpha-member", fixture.read, 0xFF03, 4, 4, 16, -1, copied));
        CHECK_FALSE(store.CopyAlphaFacts("alpha-member", fixture.read, 0xFF03, 4, 4, 16, 5, copied));
        REQUIRE(store.CopyAlphaFacts("alpha-member", fixture.read, 0xFF03, 4, 4, 16, 4, copied));
        store.MarkUploaded("alpha-member");
        CHECK_FALSE(store.HasAlphaFacts("alpha-member"));
        CHECK_FALSE(store.CopyAlphaFacts("alpha-member", fixture.read, 0xFF03, 4, 4, 16, 4, copied));
    }
    SECTION("world clear rejects publication and clears claims")
    {
        auto facts = store.BuildAlphaFacts(fixture.chain, fixture.read); REQUIRE(facts);
        store.Clear();
        CHECK_FALSE(store.Put("alpha-member", std::move(fixture.chain), generation, std::move(facts)));
        CHECK_FALSE(store.HasAlphaFacts("alpha-member"));
    }
    SECTION("same-world cancellation denies claims before physical retirement")
    {
        auto inventory = render::DdsPublicationInventory::Create(1); REQUIRE(inventory);
        const render::DdsPublicationToken token{inventory, 0, inventory->Epoch(0)};
        auto facts = store.BuildAlphaFacts(fixture.chain, fixture.read); REQUIRE(facts);
        REQUIRE(store.CanRetainAlphaFacts("alpha-member", fixture.chain, fixture.read, generation, &token));
        REQUIRE(store.Put("alpha-member", std::move(fixture.chain), generation, std::move(facts), &token));
        render::PreparedAlphaFacts priorClaim;
        REQUIRE(store.CopyAlphaFacts("alpha-member", fixture.read, 0xFF03, 4, 4, 16, 4, priorClaim));
        const auto retained = store.SnapshotStats().bytes;
        const auto before = store.SnapshotStats();
        inventory->CancelModel(0);
        CHECK_FALSE(store.HasAlphaFacts("alpha-member"));
        render::PreparedAlphaFacts rejected;
        CHECK_FALSE(store.CopyAlphaFacts("alpha-member", fixture.read, 0xFF03, 4, 4, 16, 4, rejected));
        CHECK(store.SnapshotStats().bytes == retained); // logical invisibility is not eager deletion
        Poseidon::PAABlockChain chain; CHECK_FALSE(store.Take("alpha-member", chain));
        CHECK(store.SnapshotStats().bytes == 0);
        CHECK(store.SnapshotStats().paaCancelled == before.paaCancelled + 1);
        CHECK(store.SnapshotStats().ddsCancelled == before.ddsCancelled);
        CHECK(priorClaim.histogram.pctClear > 0); // prior linearized claims remain owned values
    }
    SECTION("cancelled retained entry cannot block a new epoch for the same key")
    {
        auto inventory = render::DdsPublicationInventory::Create(1); REQUIRE(inventory);
        const render::DdsPublicationToken old{inventory, 0, inventory->Epoch(0)};
        auto chain = fixture.chain;
        auto facts = store.BuildAlphaFacts(chain, fixture.read); REQUIRE(facts);
        REQUIRE(store.Put("alpha-member", std::move(chain), generation, std::move(facts), &old));
        inventory->CancelModel(0);
        const render::DdsPublicationToken current{inventory, 0, inventory->Epoch(0)};
        const auto before = store.SnapshotStats();
        CHECK_FALSE(store.CanRetainAlphaFacts("alpha-member", fixture.chain, fixture.read, generation, &old));
        CHECK(store.SnapshotStats().alphaFactsCapacitySkipped == before.alphaFactsCapacitySkipped);
        REQUIRE(store.CanRetainAlphaFacts("alpha-member", fixture.chain, fixture.read, generation, &current));
        CHECK(store.SnapshotStats().bytes == 0); // targeted retirement releases payload outside lock
        facts = store.BuildAlphaFacts(fixture.chain, fixture.read); REQUIRE(facts);
        REQUIRE(store.Put("alpha-member", std::move(fixture.chain), generation, std::move(facts), &current));
        render::PreparedAlphaFacts copied;
        REQUIRE(store.CopyAlphaFacts("alpha-member", fixture.read, 0xFF03, 4, 4, 16, 4, copied));
    }
    SECTION("late cancelled token put fails closed without cancelling ordinary callers")
    {
        auto inventory = render::DdsPublicationInventory::Create(1); REQUIRE(inventory);
        const render::DdsPublicationToken old{inventory, 0, inventory->Epoch(0)};
        inventory->InvalidateAll();
        auto late = fixture.chain;
        auto facts = store.BuildAlphaFacts(late, fixture.read); REQUIRE(facts);
        CHECK_FALSE(store.Put("alpha-member", std::move(late), generation, std::move(facts), &old));
        CHECK_FALSE(store.HasAlphaFacts("alpha-member"));
        CHECK(store.SnapshotStats().bytes == 0);
        REQUIRE(store.Put("ordinary-member", std::move(fixture.chain))); // no token preserves original API
        Poseidon::PAABlockChain chain; REQUIRE(store.Take("ordinary-member", chain));
        CHECK(chain.blocks.size() == 16);
    }
    SECTION("sidecar capacity participates in the same retained budget")
    {
        if (store.ByteBudget() > 4 * 1024 * 1024)
            SKIP("Run with WGR_OBJECT_STREAM_TEXTURE_PREPARE_MB=1");
        const auto metadata = fixture.chain.levels.capacity() * sizeof(PAABlockLevel);
        fixture.chain.blocks.reserve(store.ByteBudget() - metadata);
        const auto before = store.SnapshotStats();
        CHECK_FALSE(store.CanRetainAlphaFacts("alpha-member", fixture.chain, fixture.read, generation));
        CHECK(store.SnapshotStats().alphaFactsCapacitySkipped == before.alphaFactsCapacitySkipped + 1);
        CHECK(store.SnapshotStats().alphaFactsCancelled == before.alphaFactsCancelled);
        auto facts = store.BuildAlphaFacts(fixture.chain, fixture.read); REQUIRE(facts);
        CHECK_FALSE(store.Put("alpha-member", std::move(fixture.chain), generation, std::move(facts)));
        CHECK(store.SnapshotStats().bytes == 0);
        CHECK_FALSE(store.HasAlphaFacts("alpha-member"));
    }
    SECTION("advisory permission never reserves against an intervening publication")
    {
        if (store.ByteBudget() > 4 * 1024 * 1024)
            SKIP("Run with WGR_OBJECT_STREAM_TEXTURE_PREPARE_MB=1");
        REQUIRE(store.CanRetainAlphaFacts("alpha-member", fixture.chain, fixture.read, generation));
        PAABlockChain competitor;
        const auto ordinary = TinyDxt1Paa();
        REQUIRE(ReadPAABlockChainBuffer(ordinary.data(), ordinary.size(), competitor));
        competitor.blocks.reserve(store.ByteBudget() - competitor.levels.capacity() * sizeof(PAABlockLevel) - 1);
        REQUIRE(store.Put("competitor", std::move(competitor), generation));
        const auto before = store.SnapshotStats();
        CHECK_FALSE(store.CanRetainAlphaFacts("alpha-member", fixture.chain, fixture.read, generation));
        CHECK(store.SnapshotStats().alphaFactsCapacitySkipped == before.alphaFactsCapacitySkipped + 1);
        // A previously granted advisory answer cannot force final acceptance.
        auto late = fixture.chain;
        auto facts = store.BuildAlphaFacts(late, fixture.read); REQUIRE(facts);
        CHECK_FALSE(store.Put("alpha-member", std::move(late), generation, std::move(facts)));
        PAABlockChain released; REQUIRE(store.Take("competitor", released));
        REQUIRE(store.CanRetainAlphaFacts("alpha-member", fixture.chain, fixture.read, generation));
        facts = store.BuildAlphaFacts(fixture.chain, fixture.read); REQUIRE(facts);
        REQUIRE(store.Put("alpha-member", std::move(fixture.chain), generation, std::move(facts)));
        render::PreparedAlphaFacts copied;
        REQUIRE(store.CopyAlphaFacts("alpha-member", fixture.read, 0xFF03, 4, 4, 16, 4, copied));
    }
    SECTION("generation duplicate and uploaded refusals are not capacity or cancellation failures")
    {
        const auto before = store.SnapshotStats();
        REQUIRE(store.CanRetainAlphaFacts("alpha-member", fixture.chain, fixture.read, generation));
        store.MarkUploaded("alpha-member");
        CHECK_FALSE(store.CanRetainAlphaFacts("alpha-member", fixture.chain, fixture.read, generation));
        store.MarkEvicted("alpha-member");
        auto chain = fixture.chain;
        REQUIRE(store.Put("ordinary-duplicate", std::move(chain), generation));
        CHECK_FALSE(store.CanRetainAlphaFacts("ordinary-duplicate", fixture.chain, fixture.read, generation));
        store.Clear();
        CHECK_FALSE(store.CanRetainAlphaFacts("alpha-member", fixture.chain, fixture.read, generation));
        CHECK(store.SnapshotStats().alphaFactsCapacitySkipped == before.alphaFactsCapacitySkipped);
        CHECK(store.SnapshotStats().alphaFactsCancelled == before.alphaFactsCancelled);
        REQUIRE(store.CanRetainAlphaFacts("alpha-member", fixture.chain, fixture.read, store.Generation()));
    }
    SECTION("TTL forbids a claim even without another worker publication")
    {
        if (store.TtlSeconds() <= 0 || store.TtlSeconds() > 0.1)
            SKIP("Run fresh with WGR_OBJECT_STREAM_TEXTURE_PREPARE_TTL_S=0.01");
        auto facts = store.BuildAlphaFacts(fixture.chain, fixture.read); REQUIRE(facts);
        REQUIRE(store.Put("alpha-member", std::move(fixture.chain), generation, std::move(facts)));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        render::PreparedAlphaFacts copied;
        CHECK_FALSE(store.CopyAlphaFacts("alpha-member", fixture.read, 0xFF03, 4, 4, 16, 4, copied));
        CHECK(store.SnapshotStats().alphaFactsMisses > 0);
    }
    store.Clear();
}
TEST_CASE("Source-bound warm chains require explicit claims and retain cancellation without alpha facts", "[warm-texture-store]")
{
    Foundation::CaptureMainThread();
    if (!ArchiveSourceBinding::SnapshotStats().enabled) SKIP("Requires fresh WGR_OBJECT_STREAM_WARM_TEXTURES=1 process.");
    AlphaArchiveFixture fixture;
    auto buffer = fixture.bank.Read("member.paa"); REQUIRE(buffer);
    auto binding = buffer->GetArchiveSourceBinding(); REQUIRE(binding);
    auto& store = render::PreparedTextureStore::Instance();
    store.Clear();
    const auto generation = store.Generation();
    auto inventory = render::DdsPublicationInventory::Create(1); REQUIRE(inventory);
    const render::DdsPublicationToken token{inventory, 0, inventory->Epoch(0)};
    auto publish = [&](uint64_t gen, const render::DdsPublicationToken* claim, bool bound = true) {
        auto chain = fixture.chain;
        return store.Put("warm-source", std::move(chain), gen, nullptr, claim, bound ? binding : nullptr);
    };
    store.MarkUploaded("warm-source");
    CHECK_FALSE(publish(generation, &token, false)); // Legacy uploaded-mark behaviour is preserved.
    CHECK(store.ShouldPrepareWarm("warm-source"));
    CHECK_FALSE(publish(generation, nullptr)); // Binding alone does not certify current demand.
    REQUIRE(publish(generation, &token));
    CHECK(store.SnapshotStats().bytes >= fixture.chain.blocks.capacity() + binding->KnownCppBytes());
    PAABlockChain output;
    const auto refusedBefore = store.SnapshotStats().warmSourceRefused;
    CHECK_FALSE(store.Take("warm-source", output)); // A provenance-unaware caller cannot consume it.
    CHECK_FALSE(output.valid()); CHECK(store.SnapshotStats().warmSourceRefused == refusedBefore + 1);
    REQUIRE(publish(generation, &token));
    std::shared_ptr<const ArchiveSourceBinding> claimed;
    REQUIRE(store.Take("warm-source", output, &claimed));
    CHECK(claimed == binding); CHECK(output.blocks == fixture.chain.blocks);
    CHECK(claimed->Request().SameArchiveMember(fixture.read));
    CHECK_FALSE(store.HasAlphaFacts("warm-source")); // Source witness is independent of optional facts.
    REQUIRE(publish(generation, &token));
    inventory->CancelModel(0);
    CHECK_FALSE(store.Take("warm-source", output, &claimed)); CHECK_FALSE(claimed);
    CHECK_FALSE(publish(generation, &token));
    const render::DdsPublicationToken replacement{inventory, 0, inventory->Epoch(0)};
    REQUIRE(publish(generation, &replacement));
    store.Clear();
    CHECK_FALSE(publish(generation, &replacement));
    REQUIRE(publish(store.Generation(), &replacement));
    REQUIRE(store.Take("warm-source", output, &claimed));
    CHECK(output.blocks == fixture.chain.blocks); CHECK(claimed == binding);
    store.Clear();
}
#endif
