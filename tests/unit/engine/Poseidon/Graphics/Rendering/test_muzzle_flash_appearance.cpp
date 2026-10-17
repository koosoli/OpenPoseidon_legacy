#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/MuzzleFlashAppearance.hpp>
#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace Poseidon::render;

TEST_CASE("Stock muzzle admission requires exact authored sheet and dimensions", "[muzzle-flash]")
{
    for (char phase = '1'; phase <= '3'; ++phase)
    {
        std::string front = "data\\zasleh_front.01.paa";
        std::string side = "DATA/ZASLEH_SIDE.01.PAA";
        std::string trident = "data/zasleh2_front.01.paa";
        std::string ball = "data/zasleh3_front.01.paa";
        std::string ballSide = "data/zasleh3_side.01.paa";
        for (std::string* name : {&front, &side, &trident, &ball, &ballSide}) (*name)[name->size() - 5] = phase;
        REQUIRE(StockMuzzleSheetAt(front, 128, 128) == StockMuzzleSheet::Front);
        REQUIRE(StockMuzzleSheetAt(side, 128, 64) == StockMuzzleSheet::Side);
        REQUIRE(StockMuzzleSheetAt(trident, 128, 128) == StockMuzzleSheet::FrontTrident);
        REQUIRE(StockMuzzleSheetAt(ball, 128, 128) == StockMuzzleSheet::FrontBall);
        REQUIRE(StockMuzzleSheetAt(ballSide, 128, 64) == StockMuzzleSheet::SideBall);
        REQUIRE(StockMuzzleSheetAt(front, 128, 64) == StockMuzzleSheet::None);
        REQUIRE(StockMuzzleSheetAt(side, 128, 128) == StockMuzzleSheet::None);
    }
    for (const char* name : {"zasleh_front.01.paa", "addon/data/zasleh_front.01.paa",
                            "data/zasleh_front.04.paa", "data/zasleh4_front.01.paa", "data/zasleh2_side.01.paa",
                            "data/fired.73.paa", "data/m4_1.pac", "data/zasleh_front.01.paa.extra"})
        REQUIRE(StockMuzzleSheetAt(name, 128, 128) == StockMuzzleSheet::None);
    REQUIRE(StockMuzzleSheetAt("data/zasleh_front.01.paa", 256, 256) == StockMuzzleSheet::None);
    for (unsigned flags = 0; flags < 16; ++flags)
        REQUIRE(StockMuzzleDrawAdmitted(StockMuzzleSheet::Front, flags & 1, flags & 2,
                                       flags & 4, flags & 8) == (flags == 15));
    REQUIRE_FALSE(StockMuzzleDrawAdmitted(StockMuzzleSheet::None, true, true, true, true));
}

namespace
{
std::vector<std::uint8_t> Generate(const std::string& name, int w, int h)
{
    std::vector<std::uint8_t> image(std::size_t(w) * h * 4, 255);
    REQUIRE(WarmStockMuzzleSheet(name, w, h, image.data(), image.size(), true));
    return image;
}
}

TEST_CASE("Procedural front flashes have a hot core, soft clear border and phase variety", "[muzzle-flash]")
{
    for (const char* family : {"data/zasleh_front.0", "data/zasleh2_front.0", "data/zasleh3_front.0"})
    {
        std::array<std::vector<std::uint8_t>, 3> frames;
        for (int phase = 0; phase < 3; ++phase)
        {
            auto& image = frames[phase];
            image = Generate(std::string(family) + std::to_string(phase + 1) + ".paa", 128, 128);
            const auto px = [&](int x, int y, int c) { return int(image[(y * 128 + x) * 4 + c]); };
            // White-hot centre: opaque, red saturated, blue high (chroma encodes heat).
            REQUIRE(px(64, 64, 3) >= 245);
            REQUIRE(px(64, 64, 0) >= 245);
            REQUIRE(px(64, 64, 2) >= 180);
            std::size_t covered = 0, opaque = 0;
            for (int y = 0; y < 128; ++y)
                for (int x = 0; x < 128; ++x)
                {
                    const int a = px(x, y, 3);
                    covered += a > 8;
                    opaque += a == 255;
                    // Heat ramp only: red >= green >= blue everywhere it is visible.
                    if (a > 8) { REQUIRE(px(x, y, 0) >= px(x, y, 1)); REQUIRE(px(x, y, 0) >= px(x, y, 2)); }
                    const float r = std::sqrt(float((x - 63.5f) * (x - 63.5f) + (y - 63.5f) * (y - 63.5f))) / 64.0f;
                    if (r > 0.99f) REQUIRE(a == 0); // nothing reaches the sheet border
                }
            REQUIRE(covered > 300);
            REQUIRE(covered < 9000);  // a flash, not a filled disc
            REQUIRE(opaque < covered / 2); // soft, gaseous falloff rather than a cut-out decal
        }
        REQUIRE(frames[0] != frames[1]);
        REQUIRE(frames[1] != frames[2]);
        REQUIRE(frames[0] != frames[2]);
        REQUIRE(Generate(std::string(family) + "1.paa", 128, 128) == frames[0]); // deterministic
    }
}

TEST_CASE("Procedural side plume is white at the muzzle end and cools along the jet", "[muzzle-flash]")
{
    for (const char* name : {"data/zasleh_side.02.paa", "data/zasleh3_side.01.paa"})
    {
        const auto image = Generate(name, 128, 64);
        const auto px = [&](int x, int y, int c) { return int(image[(y * 128 + x) * 4 + c]); };
        REQUIRE(px(115, 32, 3) >= 200);
        REQUIRE(px(115, 32, 2) >= 150); // hot (muzzle at u = 1)
        int nearHeat = 0, farHeat = 0, nearAlpha = 0, farAlpha = 0;
        for (int y = 16; y < 48; ++y)
        {
            nearHeat += px(119, y, 2); farHeat += px(67, y, 2);
            nearAlpha += px(119, y, 3); farAlpha += px(27, y, 3);
        }
        REQUIRE(nearHeat > farHeat);
        REQUIRE(nearAlpha > farAlpha);
        for (int y = 0; y < 64; ++y) REQUIRE(px(0, y, 3) == 0);
        for (int x = 0; x < 128; ++x) { REQUIRE(px(x, 0, 3) == 0); REQUIRE(px(x, 63, 3) == 0); }
    }
}

TEST_CASE("Procedural sheets refuse foreign or malformed input and honour the switch", "[muzzle-flash]")
{
    std::vector<std::uint8_t> pixels(128 * 128 * 4, 77);
    const auto original = pixels;
    REQUIRE_FALSE(WarmStockMuzzleSheet("data/zasleh_front.01.paa", 128, 128, pixels.data(), pixels.size(), false));
    REQUIRE_FALSE(WarmStockMuzzleSheet("addon/zasleh_front.01.paa", 128, 128, pixels.data(), pixels.size(), true));
    REQUIRE_FALSE(WarmStockMuzzleSheet("data/zasleh_front.01.paa", 128, 128, pixels.data(), pixels.size() - 1, true));
    REQUIRE_FALSE(WarmStockMuzzleSheet("data/zasleh_front.01.paa", 128, 128, nullptr, pixels.size(), true));
    REQUIRE(pixels == original);
}
