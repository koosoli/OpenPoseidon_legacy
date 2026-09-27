#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/LoosePaaPreparation.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace
{
struct Files
{
    std::filesystem::path directory;
    std::vector<std::filesystem::path> owned;
    Files()
    {
        static std::atomic<unsigned> sequence{0};
        directory = std::filesystem::temp_directory_path() /
            ("cwr-loose-paa-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence.fetch_add(1)));
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("private fixture directory");
    }
    ~Files()
    {
        std::error_code ec;
        for (const auto& p : owned) std::filesystem::remove(p, ec);
        std::filesystem::remove(directory, ec); // Never recursively delete a computed path.
    }
    std::string Put(const char* name, const std::vector<uint8_t>& bytes)
    {
        const auto p = directory / name;
        owned.push_back(p);
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!f) throw std::runtime_error("fixture write");
        return p.string();
    }
};
void Word(std::vector<uint8_t>& b, uint32_t value, unsigned n)
{ for (unsigned i = 0; i < n; ++i) b.push_back(static_cast<uint8_t>(value >> (8 * i))); }
std::vector<uint8_t> Paa(uint16_t magic, const std::vector<uint8_t>& pixels)
{
    std::vector<uint8_t> bytes;
    Word(bytes, magic, 2); Word(bytes, 0, 2);
    Word(bytes, 4, 2); Word(bytes, 4, 2); Word(bytes, static_cast<uint32_t>(pixels.size()), 3);
    bytes.insert(bytes.end(), pixels.begin(), pixels.end());
    Word(bytes, 0, 2); Word(bytes, 0, 2);
    return bytes;
}
}

TEST_CASE("Loose worker captures a BC chain once and preserves decoder bytes", "[loose-paa-preparation]")
{
    Files files;
    const auto bytes = Paa(0xff05, {255,0,0,0,0,0,0,0,0,248,224,7,0,0,0,0});
    auto result = Poseidon::Streaming::PrepareLoosePaa(files.Put("blocks.paa", bytes));
    REQUIRE(result.kind == Poseidon::Streaming::LoosePaaPreparation::Kind::Blocks);
    CHECK(result.openAttempts == 1); CHECK(result.capturedSources == 1); CHECK(result.capturedBytes == bytes.size());
    Poseidon::PAABlockChain expected;
    REQUIRE(Poseidon::ReadPAABlockChainBuffer(bytes.data(), bytes.size(), expected));
    CHECK(result.chain.blocks == expected.blocks);
    REQUIRE(result.chain.levels.size() == expected.levels.size());
    CHECK(result.chain.levels[0].sourceHeaderOffset == expected.levels[0].sourceHeaderOffset);
    CHECK(result.chain.width == 4); CHECK(result.chain.height == 4);
}

TEST_CASE("Normal loose worker retains legacy eligibility beyond bounded mip count", "[loose-paa-preparation]")
{
    Files files;
    std::vector<uint8_t> bytes; Word(bytes, 0xff05, 2); Word(bytes, 0, 2);
    for (unsigned i = 0; i < 33; ++i)
    {
        Word(bytes, 4, 2); Word(bytes, 4, 2); Word(bytes, 16, 3);
        bytes.insert(bytes.end(), 16, static_cast<uint8_t>(i));
    }
    Word(bytes, 0, 4);
    const auto path = files.Put("legacy-many-mips.paa", bytes);
    Poseidon::PAABlockChain legacy;
    REQUIRE(Poseidon::ReadPAABlockChain(path, legacy));
    auto compatible = Poseidon::Streaming::PrepareLoosePaa(path, SIZE_MAX);
    REQUIRE(compatible.kind == Poseidon::Streaming::LoosePaaPreparation::Kind::Blocks);
    REQUIRE(compatible.chain.levels.size() == 33);
    CHECK(compatible.chain.blocks == legacy.blocks);
    CHECK(compatible.openAttempts == 1); CHECK(compatible.capturedSources == 1);
    auto bounded = Poseidon::Streaming::PrepareLoosePaa(path);
    CHECK(bounded.kind == Poseidon::Streaming::LoosePaaPreparation::Kind::Unavailable);
    CHECK_FALSE(bounded.image.valid()); CHECK(bounded.capturedSources == 1);
}

TEST_CASE("Loose non-BC fallback decodes the same captured source once", "[loose-paa-preparation]")
{
    Files files;
    std::vector<uint8_t> pixels;
    for (unsigned i = 0; i < 16; ++i) { pixels.push_back(i); pixels.push_back(40+i); pixels.push_back(90+i); pixels.push_back(255-i); }
    const auto bytes = Paa(0x8888, pixels);
    auto result = Poseidon::Streaming::PrepareLoosePaa(files.Put("rgba.paa", bytes));
    REQUIRE(result.kind == Poseidon::Streaming::LoosePaaPreparation::Kind::Rgba);
    CHECK(result.openAttempts == 1); CHECK(result.capturedSources == 1);
    const auto expected = Poseidon::DecodePAABuffer(bytes.data(), bytes.size(), true);
    REQUIRE(expected.valid()); CHECK(result.image.rgba == expected.rgba);
    for (unsigned i = 0; i < 16; ++i) {
        CHECK(result.image.rgba[i*4] == 90+i); CHECK(result.image.rgba[i*4+3] == 255-i);
    }
}

TEST_CASE("Loose raw fallback respects the source OFFS top-level location", "[loose-paa-preparation]")
{
    Files files;
    std::vector<uint8_t> bytes;
    Word(bytes, 0x8888, 2);
    Word(bytes, 0x54414747, 4); Word(bytes, 0x4f464653, 4); Word(bytes, 4, 4); Word(bytes, 20, 4);
    Word(bytes, 0, 2);
    Word(bytes, 4, 2); Word(bytes, 4, 2); Word(bytes, 64, 3);
    bytes.insert(bytes.end(), 64, 255); Word(bytes, 0, 4);
    auto good = Poseidon::Streaming::PrepareLoosePaa(files.Put("offset.paa", bytes));
    REQUIRE(good.kind == Poseidon::Streaming::LoosePaaPreparation::Kind::Rgba);
    CHECK(good.image.rgba == Poseidon::DecodePAABuffer(bytes.data(), bytes.size(), true).rgba);
    auto shifted = bytes; shifted[14] = 24;
    CHECK(Poseidon::Streaming::PrepareLoosePaa(files.Put("offset-shifted.paa", shifted)).kind ==
          Poseidon::Streaming::LoosePaaPreparation::Kind::Unavailable);
    auto highOffset = bytes;
    highOffset[14] = 0; highOffset[15] = 0; highOffset[16] = 0; highOffset[17] = 0x80;
    CHECK_FALSE(Poseidon::Streaming::LoosePaaTopPayloadPresent(highOffset));
    CHECK(Poseidon::Streaming::PrepareLoosePaa(files.Put("offset-high.paa", highOffset)).kind ==
          Poseidon::Streaming::LoosePaaPreparation::Kind::Unavailable);
    bytes.resize(40); // Declared source offset is valid; its payload is physically truncated.
    CHECK(Poseidon::Streaming::PrepareLoosePaa(files.Put("offset-short.paa", bytes)).kind ==
          Poseidon::Streaming::LoosePaaPreparation::Kind::Unavailable);
}

TEST_CASE("Loose capture refuses missing unreadable oversized and truncated sources", "[loose-paa-preparation]")
{
    Files files;
    using Kind = Poseidon::Streaming::LoosePaaPreparation::Kind;
    auto missing = Poseidon::Streaming::PrepareLoosePaa((files.directory/"missing.paa").string());
    CHECK(missing.kind == Kind::Unavailable); CHECK(missing.openAttempts == 1); CHECK(missing.capturedSources == 0);
    auto unreadable = Poseidon::Streaming::PrepareLoosePaa(files.directory.string());
    CHECK(unreadable.kind == Kind::Unavailable); CHECK(unreadable.capturedSources == 0);
    CHECK(Poseidon::Streaming::PrepareLoosePaa(files.Put("empty.paa", {})).kind == Kind::Unavailable);
    const auto full = Paa(0x8888, std::vector<uint8_t>(64, 255));
    auto oversized = Poseidon::Streaming::PrepareLoosePaa(files.Put("limit.paa", full), full.size()-1);
    CHECK(oversized.kind == Kind::Unavailable); CHECK(oversized.capturedSources == 0);
    auto truncated = full; truncated.resize(11+32); // Declares 64, physically has 32: no zero-padded Ready.
    auto refused = Poseidon::Streaming::PrepareLoosePaa(files.Put("truncated.paa", truncated));
    CHECK(refused.kind == Kind::Unavailable); CHECK(refused.capturedSources == 1); CHECK_FALSE(refused.image.valid());
    auto marker = Paa(0x8888, std::vector<uint8_t>(64, 255));
    marker[4] = 0xd2; marker[5] = 4; marker[6] = 0x3d; marker[7] = 0x22; // Exact 1234/8765 marker.
    CHECK_FALSE(Poseidon::Streaming::LoosePaaTopPayloadPresent(marker));
    for (const auto tag : {0x41564743u, 0x464c4147u, 0x4f464653u})
    {
        std::vector<uint8_t> tagged; Word(tagged, 0x8888, 2);
        Word(tagged, 0x54414747, 4); Word(tagged, tag, 4); Word(tagged, 5, 4);
        tagged.insert(tagged.end(), 5, 0); tagged.insert(tagged.end(), full.begin()+2, full.end());
        CHECK_FALSE(Poseidon::Streaming::LoosePaaTopPayloadPresent(tagged));
        CHECK(Poseidon::Streaming::PrepareLoosePaa(files.Put(("tag-"+std::to_string(tag)+".paa").c_str(), tagged)).kind == Kind::Unavailable);
    }
    auto badBc = Paa(0xff05, std::vector<uint8_t>(16, 255)); badBc.resize(15);
    CHECK(Poseidon::Streaming::PrepareLoosePaa(files.Put("truncated-bc.paa", badBc)).kind == Kind::Unavailable);
}
