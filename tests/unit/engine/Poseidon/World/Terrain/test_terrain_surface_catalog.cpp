// TerrainSurfaceCatalog: the map's own clutter definitions, read out of an
// Arma-generation config.bin that ParamFile cannot open at all.
//
// The fixture is BUILT here rather than loaded from the corpus, so the test needs
// no licensed data and still exercises the real container: `\0raP`, the root class
// body at 0x10, LEB128 member counts, and absolute offsets for nested classes. The
// shape mirrors Takistan's actual config, including the details that bite --
// `character="Empty"` on a surface that grows nothing, probabilities that do NOT
// sum to one, and the world's `class clutter` living apart from the characters
// that reference it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/catch_approx.hpp>

#include <Poseidon/World/Terrain/TerrainSurfaceCatalog.hpp>

#include <cstring>
#include <string>
#include <vector>

using Poseidon::TerrainSurfaceCatalog;

namespace
{
// Minimal writer for the same container ArmaRapReader consumes.
struct RapBuilder
{
    std::vector<uint8_t> bytes;

    void u8(uint8_t v) { bytes.push_back(v); }
    void u32(uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            bytes.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
    }
    void str(const std::string& s)
    {
        bytes.insert(bytes.end(), s.begin(), s.end());
        bytes.push_back(0);
    }
    void count(uint32_t v)
    {
        while (true)
        {
            const uint8_t part = static_cast<uint8_t>(v & 0x7f);
            v >>= 7;
            bytes.push_back(v != 0 ? static_cast<uint8_t>(part | 0x80) : part);
            if (v == 0)
                return;
        }
    }
    // A class member whose body offset is patched once the body is written.
    size_t classRef(const std::string& name)
    {
        u8(0);
        str(name);
        const size_t patch = bytes.size();
        u32(0);
        return patch;
    }
    void patch(size_t at, uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            bytes[at + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xff);
    }
    void stringValue(const std::string& name, const std::string& text)
    {
        u8(1);
        u8(0);
        str(name);
        str(text);
    }
    void floatValue(const std::string& name, float f)
    {
        u8(1);
        u8(1);
        str(name);
        uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        u32(bits);
    }
    void floatArray(const std::string& name, const std::vector<float>& values)
    {
        u8(2);
        str(name);
        count(static_cast<uint32_t>(values.size()));
        for (float f : values)
        {
            u8(1);
            uint32_t bits = 0;
            std::memcpy(&bits, &f, sizeof(bits));
            u32(bits);
        }
    }
    void stringArray(const std::string& name, const std::vector<std::string>& values)
    {
        u8(2);
        str(name);
        count(static_cast<uint32_t>(values.size()));
        for (const std::string& s : values)
        {
            u8(0);
            str(s);
        }
    }
    void bodyHeader(uint32_t members)
    {
        str(""); // inherited class
        count(members);
    }
};

// A config shaped like Takistan's: two surfaces (one with clutter, one explicitly
// without), one character with two clutter entries at probabilities that sum to
// 0.92, and a world defining those clutter classes.
std::vector<uint8_t> BuildFixture()
{
    RapBuilder b;
    b.bytes = {0x00, 'r', 'a', 'P'};
    b.u32(0); // version
    b.u32(8);
    b.u32(0); // enum table offset, unused by the reader

    // Root body at 0x10: three classes.
    b.bodyHeader(3);
    const size_t surfacesRef = b.classRef("CfgSurfaces");
    const size_t charactersRef = b.classRef("CfgSurfaceCharacters");
    const size_t worldsRef = b.classRef("CfgWorlds");

    b.patch(surfacesRef, static_cast<uint32_t>(b.bytes.size()));
    b.bodyHeader(2);
    const size_t travaRef = b.classRef("TKTrava");
    const size_t skalaRef = b.classRef("TKSkala");
    b.patch(travaRef, static_cast<uint32_t>(b.bytes.size()));
    b.bodyHeader(2);
    b.stringValue("files", "tk_trava_*");
    b.stringValue("character", "TKGrassClutter");
    b.patch(skalaRef, static_cast<uint32_t>(b.bytes.size()));
    b.bodyHeader(2);
    b.stringValue("files", "tk_skala_*");
    b.stringValue("character", "Empty");

    b.patch(charactersRef, static_cast<uint32_t>(b.bytes.size()));
    b.bodyHeader(1);
    const size_t grassRef = b.classRef("TKGrassClutter");
    b.patch(grassRef, static_cast<uint32_t>(b.bytes.size()));
    b.bodyHeader(2);
    b.floatArray("probability", {0.89f, 0.03f});
    b.stringArray("names", {"TK_GrassDry", "TK_BrushHard"});

    b.patch(worldsRef, static_cast<uint32_t>(b.bytes.size()));
    b.bodyHeader(1);
    const size_t worldRef = b.classRef("Takistan");
    b.patch(worldRef, static_cast<uint32_t>(b.bytes.size()));
    b.bodyHeader(1);
    const size_t clutterRef = b.classRef("clutter");
    b.patch(clutterRef, static_cast<uint32_t>(b.bytes.size()));
    b.bodyHeader(2);
    const size_t dryRef = b.classRef("TK_GrassDry");
    const size_t brushRef = b.classRef("TK_BrushHard");
    b.patch(dryRef, static_cast<uint32_t>(b.bytes.size()));
    b.bodyHeader(4);
    b.stringValue("model", "ca\\plants_E\\Clutter\\c_GrassDesert_GroupSoft_EP1.p3d");
    b.floatValue("scaleMin", 0.7f);
    b.floatValue("scaleMax", 1.0f);
    b.floatValue("affectedByWind", 0.7f);
    b.patch(brushRef, static_cast<uint32_t>(b.bytes.size()));
    b.bodyHeader(1);
    b.stringValue("model", "ca\\plants_E\\Clutter\\c_BrushHard_EP1.p3d");
    return b.bytes;
}
} // namespace

TEST_CASE("TerrainSurfaceCatalog reads an Arma config ParamFile cannot open", "[terrain][config]")
{
    TerrainSurfaceCatalog catalog;
    REQUIRE(catalog.Empty());
    REQUIRE(catalog.AddConfig(BuildFixture()));

    CHECK(catalog.SurfaceCount() == 2);
    CHECK(catalog.CharacterCount() == 1);
    CHECK(catalog.ClutterCount() == 2);
}

TEST_CASE("TerrainSurfaceCatalog matches a surface texture to its character", "[terrain][config]")
{
    TerrainSurfaceCatalog catalog;
    REQUIRE(catalog.AddConfig(BuildFixture()));

    // A full authored path resolves: only the basename without extension is
    // matched, which is what `files=` globs are written against.
    CHECK(std::string(catalog.Character("ca\\takistan\\data\\tk_trava_co.paa").Data()) == "TKGrassClutter");
    CHECK(std::string(catalog.Character("tk_trava_co").Data()) == "TKGrassClutter");

    // `character="Empty"` means the surface grows nothing, and must not come back
    // as a character named "Empty" that a later lookup would then miss.
    CHECK(catalog.Character("ca\\takistan\\data\\tk_skala_co.paa").GetLength() == 0);

    // An unrelated texture matches nothing rather than falling through to the
    // first surface -- this is the guarantee that keeps OFP content clear of Arma
    // clutter even if both configs were ever loaded at once.
    CHECK(catalog.Character("eden\\tn").GetLength() == 0);
    CHECK(catalog.Character("").GetLength() == 0);
    CHECK(catalog.Character(nullptr).GetLength() == 0);
}

TEST_CASE("TerrainSurfaceCatalog joins clutter classes to their models", "[terrain][config]")
{
    TerrainSurfaceCatalog catalog;
    REQUIRE(catalog.AddConfig(BuildFixture()));

    const auto clutter = catalog.ClutterFor("TKGrassClutter");
    REQUIRE(clutter.size() == 2);

    // names[] and probability[] are positional, and the model comes from the
    // world's `class clutter` -- a different class, joined here.
    CHECK(std::string(clutter[0].name.Data()) == "TK_GrassDry");
    CHECK(clutter[0].probability == 0.89f);
    CHECK(std::string(clutter[0].model.Data()).find("c_GrassDesert_GroupSoft_EP1.p3d") != std::string::npos);
    CHECK(clutter[0].scaleMin == 0.7f);
    CHECK(clutter[0].affectedByWind == 0.7f);

    // Defaults survive where the clutter class omits them.
    CHECK(std::string(clutter[1].name.Data()) == "TK_BrushHard");
    CHECK(clutter[1].probability == 0.03f);
    CHECK(clutter[1].scaleMin == 1.0f);
    CHECK(clutter[1].scaleMax == 1.0f);

    CHECK(catalog.ClutterFor("NoSuchCharacter").empty());
    CHECK(catalog.ClutterFor("").empty());
}

TEST_CASE("TerrainSurfaceCatalog keeps coverage unnormalised", "[terrain][config]")
{
    TerrainSurfaceCatalog catalog;
    REQUIRE(catalog.AddConfig(BuildFixture()));

    // 0.89 + 0.03. The authored probabilities deliberately do not sum to one: the
    // remainder is bare ground, and that is what makes one surface read as a field
    // and another as scree. Normalising here would erase the difference.
    // Approx, because the assertion is about the SUM being unnormalised, not about
    // float exactness: 0.89f + 0.03f is 0.92000002.
    CHECK(catalog.Coverage("TKGrassClutter") == Catch::Approx(0.92f));
    CHECK(catalog.Coverage("NoSuchCharacter") == 0.0f);
}

TEST_CASE("TerrainSurfaceCatalog refuses data it cannot read", "[terrain][config]")
{
    TerrainSurfaceCatalog catalog;

    // An OFP CONFIG.BIN is version 4 with no `\0raP` modern layout; anything that
    // is not the modern container must be declined, not half-parsed.
    const std::vector<uint8_t> notRap = {'X', 'Y', 'Z', 0, 1, 2, 3, 4};
    CHECK_FALSE(catalog.AddConfig(notRap));
    CHECK(catalog.AddConfig({}) == false);
    CHECK(catalog.Empty());
}
