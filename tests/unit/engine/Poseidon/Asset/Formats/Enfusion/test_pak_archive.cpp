// test_pak_archive.cpp - ARF-001: Enfusion's IFF container and the .pak archive.
//
// Every container here is built byte by byte from the format, so nothing
// Reforger-owned is committed and the expected result is known by construction.
//
// The corpus check that matters lives outside CI, where the corpus is: all 16 .pak
// files of an Arma Reforger install pass all four closure criteria -- FORM size,
// chunk walk to EOF, directory consumed exactly, and entry extents tiling the DATA
// chunk with zero gaps and zero overlaps -- over 222,566 files and 12,263
// directories, and only two (flags, method) pairs occur: (0,0) stored and (1,6)
// zlib.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/Enfusion/EnfusionIff.hpp>
#include <Poseidon/Asset/Formats/Enfusion/TerrainTile.hpp>
#include <Poseidon/Asset/Formats/Enfusion/PakArchive.hpp>
#include <Poseidon/Asset/Formats/Enfusion/EnfusionMount.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <zlib.h>

using Poseidon::Asset::Formats::Enfusion::FourCC;
using Poseidon::Asset::Formats::Enfusion::ReadIff;
using Poseidon::Asset::Formats::Enfusion::ReadTerrainDescriptor;
using Poseidon::Asset::Formats::Enfusion::ReadTerrainTile;

namespace
{

void AppendBe32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void AppendLe32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
}

void AppendLe16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void AppendF32(std::vector<uint8_t>& out, float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    AppendLe32(out, bits);
}

void AppendTag(std::vector<uint8_t>& out, const char* tag)
{
    out.insert(out.end(), tag, tag + 4);
}

//! Wraps chunk bytes in a FORM of the given type, fixing up the size.
std::vector<uint8_t> Form(const char* type, const std::vector<uint8_t>& chunks)
{
    std::vector<uint8_t> out;
    AppendTag(out, "FORM");
    AppendBe32(out, static_cast<uint32_t>(4 + chunks.size())); // form type + chunks
    AppendTag(out, type);
    out.insert(out.end(), chunks.begin(), chunks.end());
    return out;
}

void AppendChunk(std::vector<uint8_t>& out, const char* tag, const std::vector<uint8_t>& payload)
{
    AppendTag(out, tag);
    AppendBe32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

} // namespace

TEST_CASE("PAK read requests own metadata and isolate concurrent failures", "[asset][enfusion][pak][streaming]")
{
    namespace fs = std::filesystem;
    namespace enf = Poseidon::Asset::Formats::Enfusion;
    const auto directory = fs::temp_directory_path() / ("poseidon-pak-request-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(directory));
    struct Cleanup
    {
        fs::path directory;
        ~Cleanup() { std::error_code error; fs::remove(directory / "payload.bin", error); fs::remove(directory, error); }
    } cleanup{directory};
    const auto path = directory / "payload.bin";
    const std::vector<uint8_t> expected{1, 2, 3, 4};
    uLongf size = compressBound(expected.size());
    std::vector<uint8_t> compressed(size);
    REQUIRE(compress2(compressed.data(), &size, expected.data(), expected.size(), Z_BEST_SPEED) == Z_OK);
    compressed.resize(size);
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(expected.data()), expected.size());
        file.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
        REQUIRE(file.good());
    }
    enf::PakEntry metadata{"pixels", 0, 4, 4, 0, 0, 0};
    const enf::PakReadRequest plain{path.string(), metadata};
    metadata.path = "changed after snapshot";
    metadata.realSize = 99;
    CHECK(plain.entry.path == "pixels");
    std::vector<uint8_t> out;
    std::string error = "previous error";
    REQUIRE(plain.Read(out, error));
    CHECK(out == expected);
    CHECK(error.empty());
    const enf::PakReadRequest zipped{path.string(), enf::PakEntry{"compressed", 4,
        static_cast<uint32_t>(compressed.size()), 4, 1, 6, 0}};
    REQUIRE(zipped.Read(out, error));
    CHECK(out == expected);
    auto shortDecode = zipped;
    shortDecode.entry.realSize = 8;
    CHECK_FALSE(shortDecode.Read(out, error));
    CHECK(out.empty());
    CHECK(error.find("decoded 4 of 8") != std::string::npos);
    auto missing = plain;
    missing.archivePath = (directory / "missing").string();
    CHECK_FALSE(missing.Read(out, error));
    CHECK(out.empty());
    std::vector<std::future<bool>> workers;
    for (int worker = 0; worker < 8; ++worker)
        workers.push_back(std::async(std::launch::async, [&, worker] {
            auto bad = zipped;
            bad.entry.method = 9;
            bad.entry.path = "worker-" + std::to_string(worker);
            for (int i = 0; i < 16; ++i)
            {
                std::vector<uint8_t> pixels;
                std::string why;
                if (!zipped.Read(pixels, why) || pixels != expected || !why.empty()) return false;
                if (bad.Read(pixels, why) || !pixels.empty() ||
                    why != bad.entry.path + ": unknown compression method 9") return false;
            }
            return true;
        }));
    for (auto& worker : workers) CHECK(worker.get());
}

TEST_CASE("Enfusion mount snapshots survive close and concurrent remount", "[asset][enfusion][pak][streaming][mount-lifetime]")
{
    namespace fs = std::filesystem;
    namespace enf = Poseidon::Asset::Formats::Enfusion;
    const auto directory = fs::temp_directory_path() / ("poseidon-mount-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(fs::create_directory(directory));
    struct Cleanup {
        fs::path directory;
        ~Cleanup() { std::error_code error; fs::remove(directory / "test.pak", error); fs::remove(directory, error); }
    } cleanup{directory};
    const std::vector<uint8_t> expected{1, 2, 3, 4};
    std::vector<uint8_t> index{0, 0}; // nameless directory root
    AppendLe32(index, 1);
    const std::string name = "textures/pixels.edds";
    index.push_back(1); index.push_back(static_cast<uint8_t>(name.size()));
    index.insert(index.end(), name.begin(), name.end());
    // Absolute offset: FORM header (12) plus DATA chunk header (8).
    for (uint32_t value : {20u, 4u, 4u, 0u, 0u, 0u}) AppendLe32(index, value);
    std::vector<uint8_t> chunks;
    AppendChunk(chunks, "DATA", expected);
    AppendChunk(chunks, "FILE", index);
    const auto bytes = Form("PAC1", chunks);
    {
        std::ofstream file(directory / "test.pak", std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        REQUIRE(file.good());
    }
    enf::EnfusionMount mount;
    REQUIRE(mount.Open(directory.string()));
    REQUIRE(mount.EntryCount() == 1);
    const auto oldRoot = mount.Root();
    const auto generation = mount.Generation();
    const auto request = mount.MakeReadRequest("TEXTURES\\PIXELS.EDDS");
    REQUIRE(request);
    CHECK(mount.CanonicalPath("TEXTURES\\PIXELS.EDDS") == name);
    CHECK(mount.CanonicalPath(name) == name);
    CHECK_FALSE(mount.CanonicalPath("missing.edds"));
    CHECK_FALSE(mount.CanonicalPath("enft|1,1,1|textures/pixels.edds"));
    mount.Close();
    CHECK(mount.Generation() != generation);
    CHECK(mount.Root().empty());
    CHECK(oldRoot == directory.string());
    CHECK_FALSE(mount.MakeReadRequest(name));
    CHECK_FALSE(mount.CanonicalPath(name));
    std::vector<uint8_t> pixels;
    std::string error;
    REQUIRE(request->Read(pixels, error));
    CHECK(pixels == expected);
    std::vector<std::future<bool>> workers;
    for (int w = 0; w < 4; ++w) {
        workers.push_back(std::async(std::launch::async, [&] {
            for (int i = 0; i < 128; ++i) {
                std::vector<uint8_t> out{99};
                if (mount.Read(name, out)) { if (out != expected) return false; }
                else if (!out.empty()) return false;
                if (mount.FindAll("pixels").size() > 1 || mount.EntryCount() > 1 || mount.ArchiveCount() > 1)
                    return false;
                const auto root = mount.Root();
                if (!root.empty() && root != directory.string()) return false;
            }
            return true;
        }));
    }
    for (int i = 0; i < 16; ++i) {
        REQUIRE(mount.Open(directory.string()));
        mount.Close();
    }
    for (auto& worker : workers) CHECK(worker.get());
    REQUIRE(request->Read(pixels, error));
    CHECK(pixels == expected);
}

TEST_CASE("IFF: a well-formed FORM walks to its chunks", "[asset][enfusion][arf-001]")
{
    std::vector<uint8_t> chunks;
    AppendChunk(chunks, "HEAD", std::vector<uint8_t>(28, 0));
    AppendChunk(chunks, "DATA", std::vector<uint8_t>{1, 2, 3, 4, 5});
    const auto file = Form("PAC1", chunks);

    const auto iff = ReadIff(file.data(), file.size());
    REQUIRE(iff.valid());
    REQUIRE(iff.formType == FourCC("PAC1"));
    REQUIRE(iff.chunks.size() == 2);
    REQUIRE(iff.Find(FourCC("HEAD")) != nullptr);
    REQUIRE(iff.Find(FourCC("DATA"))->size == 5);
    REQUIRE(iff.Find(FourCC("NOPE")) == nullptr);
}

TEST_CASE("IFF: the size field is big-endian", "[asset][enfusion][arf-001]")
{
    // Reading the chunk size little-endian like the payloads is the mistake that
    // works on a tiny file and diverges on a large one, so assert the byte order
    // directly: a 5-byte DATA chunk must encode as 00 00 00 05.
    std::vector<uint8_t> chunks;
    AppendChunk(chunks, "DATA", std::vector<uint8_t>{1, 2, 3, 4, 5});
    REQUIRE(chunks[4] == 0x00);
    REQUIRE(chunks[5] == 0x00);
    REQUIRE(chunks[6] == 0x00);
    REQUIRE(chunks[7] == 0x05);
}

TEST_CASE("IFF: both closures are required, not just the FORM size", "[asset][enfusion][arf-001]")
{
    // A FORM whose declared size is right but whose interior chunk overruns must
    // be refused. Checking only the outer size accepts a misaligned file.
    std::vector<uint8_t> chunks;
    AppendChunk(chunks, "DATA", std::vector<uint8_t>{1, 2, 3, 4});
    auto file = Form("PAC1", chunks);
    // Inflate the chunk's declared size without changing the file length.
    file[16] = 0x00;
    file[17] = 0x00;
    file[18] = 0x00;
    file[19] = 0x40;
    const auto iff = ReadIff(file.data(), file.size());
    REQUIRE_FALSE(iff.valid());
    REQUIRE(iff.error.find("past the end") != std::string::npos);

    // And a FORM whose declared size disagrees with the buffer.
    auto shortened = Form("PAC1", chunks);
    shortened.push_back(0xAB);
    const auto trailing = ReadIff(shortened.data(), shortened.size());
    REQUIRE_FALSE(trailing.valid());
}

TEST_CASE("TERR: the descriptor's height mapping and materials", "[asset][enfusion][arf-001]")
{
    // The scale and offset are read from the file even though they are
    // byte-identical on all 11 local terrains: "constant across the corpus I have"
    // is not "constant".
    std::vector<uint8_t> head;
    AppendLe32(head, 6401); // gridW
    AppendLe32(head, 6401); // gridH
    AppendLe32(head, 33);
    AppendLe32(head, 4); // sub-cells per tile edge
    AppendF32(head, 2.0f);
    AppendF32(head, 0.03125f);
    AppendF32(head, -204.78125f);
    AppendLe32(head, 9);

    std::vector<uint8_t> mats;
    AppendLe16(mats, 8); // the version-9 prefix
    const std::string reference = "{ABCD1234}Terrains/Common/Surfaces/Grass_03.emat";
    AppendLe32(mats, static_cast<uint32_t>(reference.size() + 1));
    mats.insert(mats.end(), reference.begin(), reference.end());
    mats.push_back(0);

    std::vector<uint8_t> chunks;
    std::vector<uint8_t> vers;
    AppendLe32(vers, 9);
    AppendChunk(chunks, "VERS", vers);
    AppendChunk(chunks, "HEAD", head);
    AppendChunk(chunks, "MATS", mats);
    const auto file = Form("TERR", chunks);

    const auto descriptor = ReadTerrainDescriptor(file.data(), file.size());
    REQUIRE(descriptor.valid());
    REQUIRE(descriptor.version == 9);
    REQUIRE(descriptor.gridWidth == 6401);
    REQUIRE(descriptor.cellSize == 2.0f);
    REQUIRE(descriptor.WorldWidth() == 12800.0f);
    // Sea level is exactly raw 6553 on this mapping, which is what makes the
    // land/water split a comparison rather than a threshold guess.
    REQUIRE(descriptor.HeightAt(6553) == 0.0f);
    REQUIRE(descriptor.HeightAt(0) == -204.78125f);
    // The GUID is stripped; the path is the half that resolves.
    REQUIRE(descriptor.materials.size() == 1);
    REQUIRE(descriptor.materials[0] == "Terrains/Common/Surfaces/Grass_03.emat");
}

TEST_CASE("TERR: a tile derives its edge from the HGHT size", "[asset][enfusion][arf-001]")
{
    // The tile edge is stored nowhere and is not constant across the corpus (128
    // cells on the shipped worlds, 256 and 64 on editor ones), so it is derived --
    // and a size that is not a perfect square of u16 must be refused rather than
    // silently producing a skewed tile.
    std::vector<uint8_t> heights;
    for (uint32_t i = 0; i < 129 * 129; ++i)
        AppendLe16(heights, static_cast<uint16_t>(i % 65536));
    std::vector<uint8_t> chunks;
    AppendChunk(chunks, "HGHT", heights);
    const auto file = Form("TERR", chunks);

    const auto tile = ReadTerrainTile(file.data(), file.size());
    REQUIRE(tile.valid());
    REQUIRE(tile.samples == 129);
    // Row-major with X fastest: sample (1, 0) is index 1, and (0, 1) is index 129.
    REQUIRE(tile.At(1, 0) == 1);
    REQUIRE(tile.At(0, 1) == 129);

    std::vector<uint8_t> ragged;
    for (uint32_t i = 0; i < 130 * 129; ++i)
        AppendLe16(ragged, 0);
    std::vector<uint8_t> raggedChunks;
    AppendChunk(raggedChunks, "HGHT", ragged);
    const auto raggedFile = Form("TERR", raggedChunks);
    const auto refused = ReadTerrainTile(raggedFile.data(), raggedFile.size());
    REQUIRE_FALSE(refused.valid());
    REQUIRE(refused.error.find("square") != std::string::npos);
}

TEST_CASE("TERR: a non-TERR form is refused", "[asset][enfusion][arf-001]")
{
    std::vector<uint8_t> chunks;
    AppendChunk(chunks, "HGHT", std::vector<uint8_t>(2, 0));
    const auto file = Form("XOB9", chunks);
    const auto tile = ReadTerrainTile(file.data(), file.size());
    REQUIRE_FALSE(tile.valid());
    REQUIRE(tile.error.find("XOB9") != std::string::npos);
}
