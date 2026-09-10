#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <chrono>
#include <vector>
#include "test_fixtures.hpp"
#include <Poseidon/Graphics/Textures/PAADecoder.hpp>
#include <stddef.h>
#include <string>
#include <vector>

using namespace Poseidon;

TEST_CASE("PAADecoder: AI88 preserves every intensity and alpha level", "[Graphics][PAADecoder][AI88]")
{
    std::vector<uint8_t> bytes;
    auto put = [&](uint32_t value, int count)
    {
        for (int i = 0; i < count; ++i)
            bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
    };
    put(0x8080, 2);
    put(0, 2); // No palette or tags.
    for (int width : {32, 16})
    {
        std::vector<uint8_t> payload;
        int32_t checksum = 0;
        for (int i = 0; i < width * 16 * 2; ++i)
        {
            if (i % 8 == 0)
                payload.push_back(0xff); // Eight literal bytes in the legacy LZSS stream.
            const uint8_t value = static_cast<uint8_t>(i % 2 == 0 ? i / 2 : 255 - i / 2);
            payload.push_back(value);
            checksum += value < 128 ? value : static_cast<int>(value) - 256;
        }
        put(width, 2);
        put(16, 2);
        put(static_cast<uint32_t>(payload.size() + 4), 3);
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        put(static_cast<uint32_t>(checksum), 4);
    }
    put(0, 4);

    auto check = [](const DecodedImage& image, int width)
    {
        REQUIRE(image.valid());
        REQUIRE(image.width == width);
        REQUIRE(image.height == 16);
        REQUIRE(image.rgba.size() == static_cast<size_t>(width * 16 * 4));
        for (int i = 0; i < width * 16; ++i)
        {
            REQUIRE(image.rgba[i * 4] == static_cast<uint8_t>(i));
            REQUIRE(image.rgba[i * 4 + 1] == static_cast<uint8_t>(i));
            REQUIRE(image.rgba[i * 4 + 2] == static_cast<uint8_t>(i));
            REQUIRE(image.rgba[i * 4 + 3] == static_cast<uint8_t>(255 - i));
        }
    };
    SECTION("buffer")
    {
        check(DecodePAABuffer(bytes.data(), bytes.size(), true), 32);
    }
    SECTION("file and selected mip")
    {
        struct TemporaryPaa
        {
            std::filesystem::path path = std::filesystem::temp_directory_path() /
                ("poseidon-ai88-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".paa");
            ~TemporaryPaa()
            {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        } file;
        std::ofstream out(file.path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        out.close();
        REQUIRE(out.good());
        check(DecodePAAFile(file.path.string()), 32);
        check(DecodePAAFileMip(file.path.string(), 1), 16);
    }
}

// Reproducers the fuzz_paa libFuzzer harness found. Pre-fix
// each was a heap-buffer-overflow in DecodePAABuffer; the dimension/payload guards
// (PAADecoder) and the LZW match bound (Pactext) now reject them.
TEST_CASE("PAADecoder: fuzz reproducers decode without out-of-bounds access", "[Graphics][PAADecoder][fuzz]")
{
    auto readAll = [](const std::string& p)
    {
        std::ifstream f(p, std::ios::binary);
        return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };

    SECTION("truncated DXT payload yields an empty image, not an OOB read")
    {
        std::vector<char> bytes = readAll(GET_FIXTURE("texture/paa/fuzz_dxt_oob.paa"));
        REQUIRE(!bytes.empty());
        DecodedImage img = DecodePAABuffer(bytes.data(), bytes.size(), true);
        // Pre-fix the DXT1 decoder read blocks past the short payload into a
        // full-size rgba; the size guard now returns an empty image instead.
        REQUIRE(img.rgba.empty());
    }

    SECTION("over-long LZW match is bounded, not written past the buffer")
    {
        std::vector<char> bytes = readAll(GET_FIXTURE("texture/paa/fuzz_lzw_overflow.paa"));
        REQUIRE(!bytes.empty());
        DecodePAABuffer(bytes.data(), bytes.size(), true); // must not overflow the output
        SUCCEED("LZW reproducer decoded without an out-of-bounds write");
    }

    SECTION("ARGB8888 payload shorter than width*height*4 yields an empty image, not an OOB read")
    {
        std::vector<char> bytes = readAll(GET_FIXTURE("texture/paa/fuzz_argb8888_oob.paa"));
        REQUIRE(!bytes.empty());
        DecodedImage img = DecodePAABuffer(bytes.data(), bytes.size(), true);
        // Pre-fix the ARGB->RGBA conversion loop read width*height*4 bytes from a
        // rawData sized to the (smaller) declared payload; the size guard now returns
        // an empty image instead of reading past rawData.
        REQUIRE(img.rgba.empty());
    }
}

// --- ReadPAAInfo tests ---

TEST_CASE("PAADecoder: ReadPAAInfo synthetic_ai88.paa header", "[Graphics][PAADecoder]")
{
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("texture/paa/synthetic_ai88.paa"), info));
    REQUIRE(info.isPaa);
    REQUIRE(info.magic == 0x8080);
    REQUIRE(info.width == 64);
    REQUIRE(info.height == 64);
    REQUIRE(std::string(info.formatName) == "AI88");
}

TEST_CASE("PAADecoder: ReadPAAInfo synthetic_argb4444.paa header", "[Graphics][PAADecoder]")
{
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("texture/paa/synthetic_argb4444.paa"), info));
    REQUIRE(info.isPaa);
    REQUIRE(info.magic == 0x4444);
    REQUIRE(info.width == 32);
    REQUIRE(info.height == 32);
    REQUIRE(std::string(info.formatName) == "ARGB4444");
}

TEST_CASE("PAADecoder: ReadPAAInfo synthetic_dxt1.paa (DXT1 64x64)", "[Graphics][PAADecoder]")
{
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("texture/paa/synthetic_dxt1.paa"), info));
    REQUIRE(info.isPaa);
    REQUIRE(info.magic == 0xFF01);
    REQUIRE(info.width == 64);
    REQUIRE(info.height == 64);
    REQUIRE(std::string(info.formatName) == "DXT1");
}

TEST_CASE("PAADecoder: ReadPAAInfo synthetic_dxt5.paa (DXT5 32x32)", "[Graphics][PAADecoder]")
{
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("paa/synthetic_dxt5.paa"), info));
    REQUIRE(info.isPaa);
    REQUIRE(info.magic == 0xFF05);
    REQUIRE(info.width == 32);
    REQUIRE(info.height == 32);
    REQUIRE(std::string(info.formatName) == "DXT5");
}

TEST_CASE("PAADecoder: ReadPAAInfo synthetic_default.pac (DXT5 64x64)", "[Graphics][PAADecoder]")
{
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("texture/pac/synthetic_default.pac"), info));
    REQUIRE_FALSE(info.isPaa);
    REQUIRE(info.magic == 0xFF05);
    REQUIRE(info.width == 64);
    REQUIRE(info.height == 64);
    REQUIRE(std::string(info.formatName) == "DXT5");
}

TEST_CASE("PAADecoder: ReadPAAInfo synthetic_dark.pac (DXT5 64x64)", "[Graphics][PAADecoder]")
{
    PAAInfo info;
    REQUIRE(ReadPAAInfo(GET_FIXTURE("texture/pac/synthetic_dark.pac"), info));
    REQUIRE_FALSE(info.isPaa);
    REQUIRE(info.magic == 0xFF05);
    REQUIRE(info.width == 64);
    REQUIRE(info.height == 64);
}

TEST_CASE("PAADecoder: ReadPAAInfo returns false for nonexistent file", "[Graphics][PAADecoder]")
{
    PAAInfo info;
    REQUIRE_FALSE(ReadPAAInfo("nonexistent_file.paa", info));
}

// --- DecodePAAFile tests ---

TEST_CASE("PAADecoder: Decode synthetic_dxt5.paa produces 32x32 RGBA", "[Graphics][PAADecoder]")
{
    auto img = DecodePAAFile(GET_FIXTURE("texture/paa/synthetic_dxt5.paa"));
    REQUIRE(img.valid());
    REQUIRE(img.width == 32);
    REQUIRE(img.height == 32);
    REQUIRE(img.rgba.size() == 32 * 32 * 4);
}

TEST_CASE("PAADecoder: Decode synthetic_dxt1.paa (DXT1) produces 64x64 RGBA", "[Graphics][PAADecoder]")
{
    auto img = DecodePAAFile(GET_FIXTURE("texture/paa/synthetic_dxt1.paa"));
    REQUIRE(img.valid());
    REQUIRE(img.width == 64);
    REQUIRE(img.height == 64);
    REQUIRE(img.rgba.size() == 64 * 64 * 4);
}

TEST_CASE("PAADecoder: Decode synthetic_dxt5.paa (DXT5) produces RGBA", "[Graphics][PAADecoder]")
{
    auto img = DecodePAAFile(GET_FIXTURE("paa/synthetic_dxt5.paa"));
    REQUIRE(img.valid());
    REQUIRE(img.width == 32);
    REQUIRE(img.height == 32);
    REQUIRE(img.rgba.size() == 32 * 32 * 4);
}

TEST_CASE("PAADecoder: Decode synthetic_default.pac (DXT5) produces 64x64 RGBA", "[Graphics][PAADecoder]")
{
    auto img = DecodePAAFile(GET_FIXTURE("texture/pac/synthetic_default.pac"));
    REQUIRE(img.valid());
    REQUIRE(img.width == 64);
    REQUIRE(img.height == 64);
    REQUIRE(img.rgba.size() == 64 * 64 * 4);
}

TEST_CASE("PAADecoder: Decode synthetic_dark.pac (DXT5) produces 64x64 RGBA", "[Graphics][PAADecoder]")
{
    auto img = DecodePAAFile(GET_FIXTURE("texture/pac/synthetic_dark.pac"));
    REQUIRE(img.valid());
    REQUIRE(img.width == 64);
    REQUIRE(img.height == 64);
    REQUIRE(img.rgba.size() == 64 * 64 * 4);
}

TEST_CASE("PAADecoder: Decode nonexistent file returns invalid", "[Graphics][PAADecoder]")
{
    auto img = DecodePAAFile("nonexistent_file.paa");
    REQUIRE_FALSE(img.valid());
}

// --- Pixel spot-checks ---

TEST_CASE("PAADecoder: synthetic DXT1 pixels decode", "[Graphics][PAADecoder]")
{
    auto img = DecodePAAFile(GET_FIXTURE("texture/paa/synthetic_dxt1.paa"));
    REQUIRE(img.valid());
    REQUIRE_FALSE(img.rgba.empty());
}

TEST_CASE("PAADecoder: DecodePAABuffer works with file data", "[Graphics][PAADecoder]")
{
    std::string path = GET_FIXTURE("texture/paa/synthetic_dxt1.paa");
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    size_t size = f.tellg();
    f.seekg(0);
    std::vector<char> buf(size);
    f.read(buf.data(), size);

    auto img = DecodePAABuffer(buf.data(), size, true);
    REQUIRE(img.valid());
    REQUIRE(img.width == 64);
    REQUIRE(img.height == 64);
}
