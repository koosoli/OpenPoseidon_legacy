// test_terrain_family.cpp -- the three terrain shader families.
//
// A terrain rvmat is an authored composition, not an object material: a satellite
// image, a selector mask, then a stack of ground surfaces. The three generations
// lay that stack out differently, and Arma 1 differs from the later two in four
// ways at once. These cases pin each difference, and in particular they pin the
// one that is silent when wrong -- Arma 1's slot assignment.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <Poseidon/Asset/Formats/Material/ShaderSchema.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <string>
#include <vector>

using namespace Poseidon::Asset::Material;

namespace
{

RvMaterialSource parse(const char* text)
{
    std::string buffer(text);
    QIStream in(buffer.data(), static_cast<int>(buffer.size()));
    ParamFile file;
    file.Parse(in);
    return ParseRvMaterial(file, "test.rvmat");
}

// Sahrani `p_030-017_l00_n_l08_n.rvmat`, transcribed. Terrain5 is 0b0101, so its
// two three-stage groups are slots 0 and 2 -- NOT 0 and 1. The filename's `n`
// tokens agree, and nothing here reads the filename.
const char* kArma1TwoOfFour = R"CFG(
PixelShaderID="Terrain5";
VertexShaderID="Terrain";
class Stage0 { texture="ca\sara\data\layers\s_030_017_lco.paa"; texGen=3; };
class Stage1 { texture="ca\sara\data\layers\m_030_017_lco.paa"; texGen=4; };
class Stage2 { texture="ca\sara\data\trava_mco.paa"; texGen=0; };
class Stage3 { texture="ca\sara\data\trava_detail_nohq.paa"; texGen=1; };
class Stage4 { texture="ca\sara\data\trava_detail_co.paa"; texGen=2; };
class Stage5 { texture="ca\sara\data\mesto_mco.paa"; texGen=0; };
class Stage6 { texture="ca\sara\data\mesto_detail_nohq.paa"; texGen=1; };
class Stage7 { texture="ca\sara\data\mesto_detail_co.paa"; texGen=2; };
class TexGen0 { uvSource="tex"; class uvTransform { aside[]={1,0,0}; up[]={0,1,0}; dir[]={0,0,1}; pos[]={0,0,0}; }; };
class TexGen1 { uvSource="tex"; class uvTransform { aside[]={10,0,0}; up[]={0,10,0}; dir[]={0,0,10}; pos[]={0,0,0}; }; };
class TexGen2 { uvSource="tex"; class uvTransform { aside[]={10,0,0}; up[]={0,10,0}; dir[]={0,0,10}; pos[]={0,0,0}; }; };
class TexGen3 { uvSource="worldPos"; class uvTransform { aside[]={0.001953,0,0}; up[]={0,0,0.001953}; dir[]={0,-0.001953,0}; pos[]={-22.46875,5.34375,0}; }; };
class TexGen4 { uvSource="worldPos"; class uvTransform { aside[]={0.001953,0,0}; up[]={0,0,0.001953}; dir[]={0,-0.001953,0}; pos[]={-22.46875,5.34375,0}; }; };
)CFG";

} // namespace

TEST_CASE("TerrainFamily: Arma 1 Terrain<N> describes a three-stage stack from Stage2", "[Material][Terrain][A1]")
{
    const auto source = parse(kArma1TwoOfFour);
    REQUIRE(source.pixelShaderId == "Terrain5");

    TerrainFamily family;
    REQUIRE(FindTerrainFamily(source.pixelShaderId, family));

    REQUIRE(family.satelliteStage == 0);
    REQUIRE(family.maskStage == 1);
    // A group is anchored on its COLOUR stage, because that is the piece every
    // generation has. Arma 1's group is macro/normal/colour on Stage2/3/4, so its
    // colour is Stage4 -- the same stage Arma 2 and Arma 3 put their first colour
    // on, reached by a different route.
    REQUIRE(family.firstSurfaceStage == 4);
    REQUIRE(family.stageStride == 3);
    // Arma 1 is the only generation with a per-surface macro at all.
    REQUIRE(family.hasMacro);
    REQUIRE(family.macroOffset == -2);
    REQUIRE(family.normalOffset == -1);
    REQUIRE(family.colourOffset == 0);
    REQUIRE(family.maxSurfaces == 4);
    REQUIRE(family.tileNormalStage == 0);
    REQUIRE(family.slotsFromShaderId);
    REQUIRE(family.slotPresenceMask == 5);
}

TEST_CASE("TerrainFamily: Arma 1 packs its groups and the shader id names their slots", "[Material][Terrain][A1]")
{
    TerrainFamily family;
    REQUIRE(FindTerrainFamily("Terrain5", family));

    // 0b0101 -> two groups, occupying slots 0 and 2. Reading the dense groups as
    // slots 0 and 1 is the silent failure this exists to prevent: the mask's green
    // channel would then select the town surface where the tile paints rock.
    REQUIRE(family.GroupCount() == 2);
    REQUIRE(family.SlotOfGroup(0) == 0);
    REQUIRE(family.SlotOfGroup(1) == 2);
    REQUIRE(family.SlotOfGroup(2) == -1);

    // Every value the corpus contains, checked as a bitmask rather than sampled.
    const int expected[16][4] = {
        {}, // 0 is not a valid shader id
        {0, -1, -1, -1},
        {1, -1, -1, -1},
        {0, 1, -1, -1},
        {2, -1, -1, -1},
        {0, 2, -1, -1},
        {1, 2, -1, -1},
        {0, 1, 2, -1},
        {3, -1, -1, -1},
        {0, 3, -1, -1},
        {1, 3, -1, -1},
        {0, 1, 3, -1},
        {2, 3, -1, -1},
        {0, 2, 3, -1},
        {1, 2, 3, -1},
        {0, 1, 2, 3},
    };
    for (int n = 1; n <= 15; ++n)
    {
        TerrainFamily probe;
        INFO("Terrain" << n);
        REQUIRE(FindTerrainFamily("Terrain" + std::to_string(n), probe));
        for (int group = 0; group < 4; ++group)
            REQUIRE(probe.SlotOfGroup(group) == expected[n][group]);
    }
}

TEST_CASE("TerrainFamily: the later generations keep stage position as the slot", "[Material][Terrain]")
{
    // Arma 2 and Arma 3 leave a stage EMPTY for an unused slot, so the slot is the
    // stage's own position and SlotOfGroup is the identity. Applying Arma 1's
    // packing rule to them would renumber every slot after a hole -- the defect
    // WLD-019 recorded on Takistan's 4,796 holes.
    TerrainFamily a2;
    REQUIRE(FindTerrainFamily("TerrainX", a2));
    REQUIRE(a2.firstSurfaceStage == 4);
    REQUIRE(a2.stageStride == 2);
    // Colour on the even stage, its normal on the ODD stage before it: 3/4, 5/6,
    // ... 13/14. Six colours at Stage4..Stage14.
    REQUIRE(a2.normalOffset == -1);
    REQUIRE(a2.colourOffset == 0);
    REQUIRE_FALSE(a2.hasMacro);
    REQUIRE(a2.maxSurfaces == 6);
    for (int group = 0; group < 6; ++group)
        REQUIRE(a2.firstSurfaceStage + group * a2.stageStride == 4 + group * 2);
    REQUIRE(a2.tileNormalStage == 0);
    REQUIRE_FALSE(a2.slotsFromShaderId);
    for (int group = 0; group < 6; ++group)
        REQUIRE(a2.SlotOfGroup(group) == group);

    TerrainFamily a3;
    REQUIRE(FindTerrainFamily("TerrainSNX", a3));
    REQUIRE(a3.firstSurfaceStage == 4);
    REQUIRE(a3.stageStride == 2);
    REQUIRE(a3.maxSurfaces == 5);
    // Arma 3 spends the stage Arma 2 gives its sixth surface on a whole-tile normal.
    REQUIRE(a3.tileNormalStage == 14);
    REQUIRE_FALSE(a3.slotsFromShaderId);
}

TEST_CASE("TerrainFamily: the mask suffix differs and only the stage index can tell", "[Material][Terrain][A1]")
{
    // Arma 1 names its mask `m_<x>_<y>_lco.paa` -- the SAME suffix as its
    // satellite `s_<x>_<y>_lco.paa`. A suffix test binds the mask as a second
    // satellite and loses the selector entirely; the stage index is the only
    // discriminator, and this is what says so.
    TerrainFamily a1, a3;
    REQUIRE(FindTerrainFamily("Terrain1", a1));
    REQUIRE(FindTerrainFamily("TerrainSNX", a3));
    REQUIRE(std::string(a1.maskSuffix) == "_lco.paa");
    REQUIRE(std::string(a3.maskSuffix) == "_lca.paa");

    const auto source = parse(kArma1TwoOfFour);
    const RvStage* satellite = source.FindStage(a1.satelliteStage);
    const RvStage* mask = source.FindStage(a1.maskStage);
    REQUIRE(satellite != nullptr);
    REQUIRE(mask != nullptr);
    REQUIRE(satellite->texture.raw == "ca\\sara\\data\\layers\\s_030_017_lco.paa");
    REQUIRE(mask->texture.raw == "ca\\sara\\data\\layers\\m_030_017_lco.paa");
    // Both end `_lco.paa`. That is the whole point.
    REQUIRE(satellite->texture.raw.substr(satellite->texture.raw.size() - 8) == "_lco.paa");
    REQUIRE(mask->texture.raw.substr(mask->texture.raw.size() - 8) == "_lco.paa");
}

TEST_CASE("TerrainFamily: Arma 1's surface stack resolves to the right textures", "[Material][Terrain][A1]")
{
    const auto source = parse(kArma1TwoOfFour);
    TerrainFamily family;
    REQUIRE(FindTerrainFamily(source.pixelShaderId, family));

    struct Expected
    {
        int slot;
        const char* macro;
        const char* normal;
        const char* colour;
    };
    const Expected expected[] = {
        {0, "ca\\sara\\data\\trava_mco.paa", "ca\\sara\\data\\trava_detail_nohq.paa",
         "ca\\sara\\data\\trava_detail_co.paa"},
        {2, "ca\\sara\\data\\mesto_mco.paa", "ca\\sara\\data\\mesto_detail_nohq.paa",
         "ca\\sara\\data\\mesto_detail_co.paa"},
    };

    REQUIRE(family.GroupCount() == 2);
    for (int group = 0; group < family.GroupCount(); ++group)
    {
        const int base = family.firstSurfaceStage + group * family.stageStride;
        INFO("group " << group);
        // Colours on Stage4 and Stage7, not Stage2 and Stage5.
        REQUIRE(base == 4 + group * 3);
        REQUIRE(family.SlotOfGroup(group) == expected[group].slot);
        REQUIRE(source.FindStage(base + family.macroOffset)->texture.raw == expected[group].macro);
        REQUIRE(source.FindStage(base + family.normalOffset)->texture.raw == expected[group].normal);
        REQUIRE(source.FindStage(base + family.colourOffset)->texture.raw == expected[group].colour);
    }
}

TEST_CASE("TerrainFamily: Arma 1's macro and detail sit at different UV scales", "[Material][Terrain][A1]")
{
    // The lineage point. Arma 1 samples a surface's `_mco` macro colour at TexGen0
    // (scale 1, one repeat per land cell) and its `_nohq`/`_co` detail pair at
    // TexGen1/TexGen2 (scale 10). One low-frequency colour, one high-frequency
    // grain, per surface, in the same material -- which is the composite Arma 2
    // and Arma 3 achieve by promoting the colour to a world-scale satellite and
    // dropping `_mco`. It is direct evidence for the modulation direction that
    // WLD-019 amendment 2 had to infer.
    const auto source = parse(kArma1TwoOfFour);
    const auto scaleOfTexGen = [&](int index)
    {
        for (const auto& texGen : source.texGens)
            if (texGen.index == index)
                return texGen.uvTransform.aside[0];
        return 0.0f;
    };
    REQUIRE(scaleOfTexGen(0) == 1.0f);
    REQUIRE(scaleOfTexGen(1) == 10.0f);
    REQUIRE(scaleOfTexGen(2) == 10.0f);

    // And the satellite/mask TexGens are `worldPos`, at 1/512 -- one satellite
    // tile per 512 m. Same convention as Arma 3's, checked rather than assumed.
    for (const int index : {3, 4})
    {
        INFO("TexGen" << index);
        const RvTexGen* texGen = nullptr;
        for (const auto& candidate : source.texGens)
            if (candidate.index == index)
                texGen = &candidate;
        REQUIRE(texGen != nullptr);
        REQUIRE(texGen->uvSource == "worldPos");
        // A BASIS, not row vectors: aside/up/dir are where the source's X/Y/Z axes
        // go. u takes the first component of each, v the second. Reading it
        // row-wise takes v from `aside`, which is zero here, and the satellite
        // collapses to a single line -- the defect WLD-019 found on Stratis.
        REQUIRE(texGen->uvTransform.aside[0] > 0.0f);
        REQUIRE(texGen->uvTransform.aside[1] == 0.0f);
        REQUIRE(texGen->uvTransform.up[0] == 0.0f);
        REQUIRE(texGen->uvTransform.up[2] > 0.0f);
        REQUIRE(texGen->uvTransform.dir[1] < 0.0f);
    }
}

TEST_CASE("TerrainFamily: non-terrain and malformed shader ids are refused", "[Material][Terrain]")
{
    TerrainFamily family;
    // "Terrain" alone is Arma 1's VERTEX shader id. Accepting it would give a slot
    // presence mask of 0 and silently bind nothing.
    REQUIRE_FALSE(FindTerrainFamily("Terrain", family));
    REQUIRE_FALSE(FindTerrainFamily("Terrain0", family));
    REQUIRE_FALSE(FindTerrainFamily("Terrain16", family));
    REQUIRE_FALSE(FindTerrainFamily("Terrain1x", family));
    REQUIRE_FALSE(FindTerrainFamily("Super", family));
    REQUIRE_FALSE(FindTerrainFamily("Multi", family));
    REQUIRE_FALSE(FindTerrainFamily("", family));
    // Case-insensitive, like every other family lookup here.
    REQUIRE(FindTerrainFamily("terrainx", family));
    REQUIRE(FindTerrainFamily("terrainsnx", family));
}
