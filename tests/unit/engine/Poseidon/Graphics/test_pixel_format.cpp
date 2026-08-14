#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Textures/PixelFormat.hpp>
#include <cstring>

using namespace Poseidon;

// --- Registry coverage ---

TEST_CASE("PixelFormatRegistry: all formats have info", "[Graphics][PixelFormat]")
{
    // 10 Arma-era formats plus the four the DXGI/DX10 header reaches: BC4, BC5,
    // BC7 and the single-channel R8 (ARF-001).
    const auto count = PixelFormatRegistry::AllFormatsCount();
    REQUIRE(count == 14);

    const auto* formats = PixelFormatRegistry::AllFormats();
    for (size_t i = 0; i < count; ++i)
    {
        REQUIRE(formats[i].format != PixelFormat::Unknown);
        REQUIRE(formats[i].name != nullptr);
        REQUIRE(formats[i].name[0] != '\0');
        REQUIRE(formats[i].description != nullptr);
        REQUIRE(formats[i].bitsPerPixel > 0);
    }
}

TEST_CASE("PixelFormatRegistry: Get returns correct info for each format", "[Graphics][PixelFormat]")
{
    SECTION("DXT1")
    {
        const auto& info = PixelFormatRegistry::Get(PixelFormat::DXT1);
        REQUIRE(info.format == PixelFormat::DXT1);
        REQUIRE(std::strcmp(info.name, "DXT1") == 0);
        REQUIRE(info.bitsPerPixel == 4);
        REQUIRE(info.isCompressed);
        REQUIRE(info.paaMagic == 0xFF01);
    }

    SECTION("ARGB4444")
    {
        const auto& info = PixelFormatRegistry::Get(PixelFormat::ARGB4444);
        REQUIRE(info.format == PixelFormat::ARGB4444);
        REQUIRE(std::strcmp(info.name, "ARGB4444") == 0);
        REQUIRE(info.bitsPerPixel == 16);
        REQUIRE_FALSE(info.isCompressed);
        REQUIRE(info.hasAlpha);
        REQUIRE(info.paaMagic == 0x4444);
    }

    SECTION("RGBA8888")
    {
        const auto& info = PixelFormatRegistry::Get(PixelFormat::RGBA8888);
        REQUIRE(info.format == PixelFormat::RGBA8888);
        REQUIRE(info.bitsPerPixel == 32);
        REQUIRE_FALSE(info.isCompressed);
        REQUIRE(info.hasAlpha);
    }

    SECTION("P8")
    {
        const auto& info = PixelFormatRegistry::Get(PixelFormat::P8);
        REQUIRE(info.format == PixelFormat::P8);
        REQUIRE(info.bitsPerPixel == 8);
        REQUIRE_FALSE(info.isCompressed);
        REQUIRE_FALSE(info.hasAlpha);
    }

    SECTION("RGB565")
    {
        const auto& info = PixelFormatRegistry::Get(PixelFormat::RGB565);
        REQUIRE(info.format == PixelFormat::RGB565);
        REQUIRE(info.bitsPerPixel == 16);
        REQUIRE_FALSE(info.hasAlpha);
    }
}

TEST_CASE("PixelFormatRegistry: Get returns Unknown for invalid format", "[Graphics][PixelFormat]")
{
    const auto& info = PixelFormatRegistry::Get(PixelFormat::Unknown);
    REQUIRE(info.format == PixelFormat::Unknown);
    REQUIRE(std::strcmp(info.name, "Unknown") == 0);

    const auto& info2 = PixelFormatRegistry::Get(PixelFormat::Count);
    REQUIRE(info2.format == PixelFormat::Unknown);
}

// --- FindByName ---

TEST_CASE("PixelFormatRegistry: FindByName exact match", "[Graphics][PixelFormat]")
{
    const auto* info = PixelFormatRegistry::FindByName("DXT1");
    REQUIRE(info != nullptr);
    REQUIRE(info->format == PixelFormat::DXT1);

    const auto* info2 = PixelFormatRegistry::FindByName("ARGB1555");
    REQUIRE(info2 != nullptr);
    REQUIRE(info2->format == PixelFormat::ARGB1555);
}

TEST_CASE("PixelFormatRegistry: FindByName case-insensitive", "[Graphics][PixelFormat]")
{
    const auto* info = PixelFormatRegistry::FindByName("dxt5");
    REQUIRE(info != nullptr);
    REQUIRE(info->format == PixelFormat::DXT5);

    const auto* info2 = PixelFormatRegistry::FindByName("Argb4444");
    REQUIRE(info2 != nullptr);
    REQUIRE(info2->format == PixelFormat::ARGB4444);

    const auto* info3 = PixelFormatRegistry::FindByName("rgba8888");
    REQUIRE(info3 != nullptr);
    REQUIRE(info3->format == PixelFormat::RGBA8888);
}

TEST_CASE("PixelFormatRegistry: FindByName returns nullptr for unknown", "[Graphics][PixelFormat]")
{
    REQUIRE(PixelFormatRegistry::FindByName("NONEXISTENT") == nullptr);
    REQUIRE(PixelFormatRegistry::FindByName("") == nullptr);
    REQUIRE(PixelFormatRegistry::FindByName(nullptr) == nullptr);
}

// --- FindByPaaMagic ---

TEST_CASE("PixelFormatRegistry: FindByPaaMagic", "[Graphics][PixelFormat]")
{
    SECTION("DXT magics")
    {
        const auto* dxt1 = PixelFormatRegistry::FindByPaaMagic(0xFF01);
        REQUIRE(dxt1 != nullptr);
        REQUIRE(dxt1->format == PixelFormat::DXT1);

        const auto* dxt3 = PixelFormatRegistry::FindByPaaMagic(0xFF03);
        REQUIRE(dxt3 != nullptr);
        REQUIRE(dxt3->format == PixelFormat::DXT3);

        const auto* dxt5 = PixelFormatRegistry::FindByPaaMagic(0xFF05);
        REQUIRE(dxt5 != nullptr);
        REQUIRE(dxt5->format == PixelFormat::DXT5);
    }

    SECTION("Uncompressed magics")
    {
        const auto* ai88 = PixelFormatRegistry::FindByPaaMagic(0x8080);
        REQUIRE(ai88 != nullptr);
        REQUIRE(ai88->format == PixelFormat::AI88);

        const auto* a4444 = PixelFormatRegistry::FindByPaaMagic(0x4444);
        REQUIRE(a4444 != nullptr);
        REQUIRE(a4444->format == PixelFormat::ARGB4444);

        const auto* a1555 = PixelFormatRegistry::FindByPaaMagic(0x1555);
        REQUIRE(a1555 != nullptr);
        REQUIRE(a1555->format == PixelFormat::ARGB1555);
    }

    SECTION("Invalid magic returns nullptr")
    {
        REQUIRE(PixelFormatRegistry::FindByPaaMagic(0x0000) == nullptr);
        REQUIRE(PixelFormatRegistry::FindByPaaMagic(0xDEAD) == nullptr);
    }
}

// --- Compressed format checks ---

TEST_CASE("PixelFormatRegistry: compressed formats", "[Graphics][PixelFormat]")
{
    REQUIRE(PixelFormatRegistry::Get(PixelFormat::DXT1).isCompressed);
    REQUIRE(PixelFormatRegistry::Get(PixelFormat::DXT3).isCompressed);
    REQUIRE(PixelFormatRegistry::Get(PixelFormat::DXT5).isCompressed);

    REQUIRE_FALSE(PixelFormatRegistry::Get(PixelFormat::P8).isCompressed);
    REQUIRE_FALSE(PixelFormatRegistry::Get(PixelFormat::AI88).isCompressed);
    REQUIRE_FALSE(PixelFormatRegistry::Get(PixelFormat::ARGB4444).isCompressed);
    REQUIRE_FALSE(PixelFormatRegistry::Get(PixelFormat::RGBA8888).isCompressed);
}

// --- FormatCount ---

TEST_CASE("PixelFormatRegistry: FormatCount matches AllFormatsCount", "[Graphics][PixelFormat]")
{
    REQUIRE(PixelFormatRegistry::FormatCount() == static_cast<int>(PixelFormatRegistry::AllFormatsCount()));
    REQUIRE(PixelFormatRegistry::FormatCount() == 14);
}
