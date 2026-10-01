#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Graphics/Rendering/Lighting/MuzzleFlashAppearance.hpp>
#include <array>
#include <string>
#include <vector>

using namespace Poseidon::render;

TEST_CASE("Stock muzzle admission requires exact authored sheet and dimensions", "[muzzle-flash]")
{
    for (char phase = '1'; phase <= '3'; ++phase)
    {
        std::string front = "data\\zasleh_front.01.paa";
        std::string side = "DATA/ZASLEH_SIDE.01.PAA";
        front[front.size() - 5] = phase;
        side[side.size() - 5] = phase;
        REQUIRE(StockMuzzleSheetAt(front, 128, 128) == StockMuzzleSheet::Front);
        REQUIRE(StockMuzzleSheetAt(side, 128, 64) == StockMuzzleSheet::Side);
        REQUIRE(StockMuzzleSheetAt(front, 128, 64) == StockMuzzleSheet::None);
    }
    for (const char* name : {"zasleh_front.01.paa", "addon/data/zasleh_front.01.paa",
                            "data/zasleh_front.04.paa", "data/zasleh2_front.01.paa",
                            "data/fired.73.paa", "data/m4_1.pac", "data/zasleh_front.01.paa.extra"})
        REQUIRE(StockMuzzleSheetAt(name, 128, 128) == StockMuzzleSheet::None);
    REQUIRE(StockMuzzleSheetAt("data/zasleh_front.01.paa", 256, 256) == StockMuzzleSheet::None);
    for (unsigned flags = 0; flags < 16; ++flags)
        REQUIRE(StockMuzzleDrawAdmitted(StockMuzzleSheet::Front, flags & 1, flags & 2,
                                       flags & 4, flags & 8) == (flags == 15));
    REQUIRE_FALSE(StockMuzzleDrawAdmitted(StockMuzzleSheet::None, true, true, true, true));
}

TEST_CASE("Muzzle warming bounds authorial alpha and filters warm edges", "[muzzle-flash]")
{
    std::vector<std::uint8_t> pixels(128 * 128 * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4)
    {
        pixels[i] = std::uint8_t(i / 4);
        pixels[i + 1] = 255;
        pixels[i + 2] = 240;
        pixels[i + 3] = std::uint8_t(i / 4);
    }
    const auto original = pixels;
    REQUIRE(WarmStockMuzzleSheet("data/zasleh_front.02.paa", 128, 128, pixels.data(), pixels.size(), true));
    for (std::size_t i = 0; i < pixels.size(); i += 4)
    {
        REQUIRE(pixels[i + 3] <= original[i + 3]);
        if (original[i + 3] == 0) REQUIRE(pixels[i + 3] == 0);
        REQUIRE(pixels[i] >= pixels[i + 1]);
        REQUIRE(pixels[i + 1] >= pixels[i + 2]);
    }
    REQUIRE(pixels[4 * 255 + 2] < 20); // a dense outer petal is not a white hot core
    REQUIRE(pixels[4 * 17 + 2] < 20); // cooler low-density lobe
    REQUIRE(pixels[0 + 2] < 20); // bilinear clear edge RGB cannot bleed green
    pixels = original;
    REQUIRE_FALSE(WarmStockMuzzleSheet("data/zasleh_front.01.paa", 128, 128, pixels.data(), pixels.size(), false));
    REQUIRE(pixels == original);
    REQUIRE_FALSE(WarmStockMuzzleSheet("addon/zasleh_front.01.paa", 128, 128, pixels.data(), pixels.size(), true));
    REQUIRE_FALSE(WarmStockMuzzleSheet("data/zasleh_front.01.paa", 128, 128, pixels.data(), pixels.size() - 1, true));
    REQUIRE_FALSE(WarmStockMuzzleSheet("data/zasleh_front.01.paa", 128, 128, nullptr, pixels.size(), true));
    REQUIRE(pixels == original);
    std::fill(pixels.begin(), pixels.end(), 0);
    REQUIRE(WarmStockMuzzleSheet("data/zasleh_front.01.paa", 128, 128, pixels.data(), pixels.size(), true));
    for (auto value : pixels) REQUIRE(value == 0); // no authored energy, no new flame
}

TEST_CASE("Muzzle core stays hot while outer silhouette tapers asymmetrically by phase", "[muzzle-flash]")
{
    std::array<std::vector<std::uint8_t>,3> frames;
    for (int phase=0;phase<3;++phase)
    {
        auto& image=frames[phase];image.assign(128*128*4,255);
        const std::string name="data/zasleh_front.0"+std::to_string(phase+1)+".paa";
        REQUIRE(WarmStockMuzzleSheet(name,128,128,image.data(),image.size(),true));
        const auto pixel=[&](int x,int y,int c){return image[(y*128+x)*4+c];};
        REQUIRE(pixel(64,64,3)==255);
        REQUIRE(pixel(64,64,0)==255);
        REQUIRE(pixel(64,64,1)>=244);
        REQUIRE(pixel(64,64,2)>=200);
        REQUIRE(pixel(127,64,3)==0);
        REQUIRE(pixel(0,64,3)==0);
        REQUIRE(pixel(64,0,3)==0);
        REQUIRE(pixel(64,127,3)==0);
        // Reflection symmetry is the original flower's salient failure. The
        // candidate must not merely reduce every spoke by an equal radius.
        std::size_t asymmetric=0, covered=0;
        for(int y=0;y<128;++y) for(int x=0;x<128;++x) {
            asymmetric+=pixel(x,y,3)!=pixel(127-x,127-y,3);
            covered+=pixel(x,y,3)>0;
        }
        REQUIRE(asymmetric>1000);
        REQUIRE(covered>1000);
        REQUIRE(covered<8000); // compact envelope even with completely opaque source
    }
    REQUIRE(frames[0]!=frames[1]);REQUIRE(frames[1]!=frames[2]);REQUIRE(frames[0]!=frames[2]);
    auto unchanged=frames[0];
    REQUIRE_FALSE(WarmStockMuzzleSheet("data/zasleh_front.01.paa",128,128,
        unchanged.data(),unchanged.size(),false));
    REQUIRE(unchanged==frames[0]);
}
