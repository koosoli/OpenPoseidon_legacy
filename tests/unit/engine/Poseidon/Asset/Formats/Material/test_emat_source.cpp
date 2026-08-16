// test_emat_source.cpp - DZ-002: Enfusion's .emat material.
//
// The materials here are written out in full rather than loaded from fixtures,
// because DayZ's data is proprietary and none of it may be committed. Every shape
// asserted below was censused across all 207 .emat files in a retail install; the
// corpus check that matters lives outside CI, where `PoseidonTools model emat` over
// that install parses 207 of 207 and reads 2,198 properties and 314 texture
// references.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/Material/EmatSource.hpp>

#include <string>

using Poseidon::Asset::Material::EmatMaterial;
using Poseidon::Asset::Material::EmatPathForRvmat;
using Poseidon::Asset::Material::ParseEmat;

TEST_CASE("EMAT: the common header form", "[asset][material][emat][dz-002]")
{
    // 164 of the 207 files open this way.
    const EmatMaterial material = ParseEmat("MatWaterPool {\n Color 0.25 0.5 0.75 1\n ZWrite 0\n}\n");

    REQUIRE(material.valid());
    REQUIRE(material.className == "MatWaterPool");
    REQUIRE(material.declaredPath.empty());
    REQUIRE(material.properties.size() == 2);

    float colour[4] = {};
    REQUIRE(material.Vec4Of("Color", colour));
    REQUIRE(colour[0] == 0.25f);
    REQUIRE(colour[3] == 1.0f);
    REQUIRE(material.FloatOr("ZWrite", 99.0f) == 0.0f);
}

TEST_CASE("EMAT: the postprocess header form", "[asset][material][emat][dz-002]")
{
    // The other 43 name a path and put the brace on its own line. Read only the
    // first form, these files parse as "no class" and vanish from any census.
    const EmatMaterial material = ParseEmat("material \"graphics/materials/postprocess/colors\": ColorsEffect\n"
                                            "{\n\tBrightness 1\n\tContrast 1\n}\n");

    REQUIRE(material.valid());
    REQUIRE(material.className == "ColorsEffect");
    REQUIRE(material.declaredPath == "graphics/materials/postprocess/colors");
    REQUIRE(material.properties.size() == 2);
    REQUIRE(material.FloatOr("Brightness", 0.0f) == 1.0f);
}

TEST_CASE("EMAT: a texture reference keeps its path and sheds its GUID", "[asset][material][emat][dz-002]")
{
    // All 314 references in the corpus carry a brace-delimited Enfusion asset id.
    // This engine has no registry to resolve one against; the path beside it is
    // what a file server can actually open.
    const EmatMaterial material =
        ParseEmat("MatWaterPool {\n"
                  " NormalMap \"{C57E538AD376F42C}DZ/water_bliss/river/data/enoch_river_nohq.edds\"\n"
                  "}\n");

    REQUIRE(material.valid());
    REQUIRE(material.TextureOf("NormalMap") == "DZ/water_bliss/river/data/enoch_river_nohq.edds");

    const auto* property = material.Find("NormalMap");
    REQUIRE(property != nullptr);
    REQUIRE(property->values.size() == 1);
    REQUIRE(property->values[0].isTexture());
    REQUIRE(property->values[0].guid == "C57E538AD376F42C");
    REQUIRE_FALSE(property->values[0].isNumber);
}

TEST_CASE("EMAT: keys may be quoted when they contain spaces", "[asset][material][emat][dz-002]")
{
    // Five such keys exist across the corpus. Split on whitespace naively and
    // "Enable effect 1" becomes a key "Enable" with values "effect" and 1.
    const EmatMaterial material = ParseEmat("MatWaterPool {\n \"Enable effect\" 1\n \"Pixel stride_1\" 16\n}\n");

    REQUIRE(material.valid());
    REQUIRE(material.properties.size() == 2);
    REQUIRE(material.properties[0].name == "Enable effect");
    REQUIRE(material.FloatOr("Enable effect", 0.0f) == 1.0f);
    REQUIRE(material.FloatOr("Pixel stride_1", 0.0f) == 16.0f);
}

TEST_CASE("EMAT: a trailing bare word is kept beside its texture", "[asset][material][emat][dz-002]")
{
    // `AlbedoMap "{...}sprite.edds" alpha` -- 11 particle materials write this.
    const EmatMaterial material =
        ParseEmat("ParticleSprite {\n AlbedoMap \"{016B7879D4775122}Graphics/Particles/sprites/spark.edds\" alpha\n"
                  " Sort translucent\n}\n");

    REQUIRE(material.valid());
    REQUIRE(material.TextureOf("AlbedoMap") == "Graphics/Particles/sprites/spark.edds");
    REQUIRE(material.WordOf("AlbedoMap") == "alpha");
    REQUIRE(material.WordOf("Sort") == "translucent");
    // A bare word is not a number, and must not be read as zero.
    float unused = -1.0f;
    REQUIRE_FALSE(material.FloatOf("Sort", unused));
    REQUIRE(unused == -1.0f);
}

TEST_CASE("EMAT: lookups are case-insensitive and absence is distinguishable", "[asset][material][emat][dz-002]")
{
    const EmatMaterial material = ParseEmat("MatWaterPool {\n WindInfl 0.2\n}\n");

    REQUIRE(material.FloatOr("windinfl", 0.0f) == 0.2f);
    REQUIRE(material.Has("WINDINFL"));
    REQUIRE_FALSE(material.Has("StreamSpeedInfl"));
    // A missing key leaves the caller's value alone rather than zeroing it.
    float speed = 1.5f;
    REQUIRE_FALSE(material.FloatOf("StreamSpeedInfl", speed));
    REQUIRE(speed == 1.5f);
    REQUIRE(material.TextureOf("WaterStreamMap").empty());
}

TEST_CASE("EMAT: a river material yields its flow parameters", "[asset][material][emat][dz-002]")
{
    // The reason this parser exists. The .rvmat beside a DayZ river is a 79-byte
    // stub naming only the CalmWater shader ids; everything that makes water look
    // like water -- and the flow field the rivers need -- is here instead.
    const EmatMaterial material =
        ParseEmat("MatWaterPool {\n"
                  " Color 0.0392 0.0549 0.0667 0.3725\n"
                  " WindInfl 0.2\n"
                  " WaterStreamMap \"{EF177E7C8B896505}DZ/water_bliss/river/data/enoch_river_flowmap_co.edds\"\n"
                  " StreamSpeedInfl 0.15\n"
                  " StreamNormalPower 2\n"
                  "}\n");

    REQUIRE(material.valid());
    REQUIRE(material.TextureOf("WaterStreamMap") == "DZ/water_bliss/river/data/enoch_river_flowmap_co.edds");
    REQUIRE(material.FloatOr("StreamSpeedInfl", 0.0f) == 0.15f);
    REQUIRE(material.FloatOr("StreamNormalPower", 0.0f) == 2.0f);

    // A pond authors no stream keys at all, which is how the two are told apart.
    const EmatMaterial pond = ParseEmat("MatWaterPool {\n Color 0 0 0 1\n WindInfl 0.1\n TimeMul 0.7\n}\n");
    REQUIRE(pond.valid());
    REQUIRE_FALSE(pond.Has("WaterStreamMap"));
}

TEST_CASE("EMAT: a nested block does not close the material", "[asset][material][emat][dz-002]")
{
    // Two of the 207 files nest a block, both of them Sakhal's ice-lake water.
    // Read flat, the inner brace closes the material and everything after it is
    // dropped silently -- which is what happened: those two files lost half their
    // texture references, and only a whole-corpus count showed it.
    const EmatMaterial material = ParseEmat("MatWaterPool {\n"
                                            " AlbedoMap \"{25A9608CDDECF90F}DZ/water_sakhal/data/ice_ca.edds\"\n"
                                            " AlbedoMapTexMat {\n"
                                            "  1 1 0 0\n"
                                            " }\n"
                                            " CausticMap \"{EA764AD7A6112284}Graphics/Textures/Water/caustic.edds\"\n"
                                            " WetFloor 0.11\n"
                                            "}\n");

    REQUIRE(material.valid());
    // The keys after the nested block must survive.
    REQUIRE(material.TextureOf("CausticMap") == "Graphics/Textures/Water/caustic.edds");
    REQUIRE(material.FloatOr("WetFloor", 0.0f) == 0.11f);

    // And the block's own contents become values of the property that opened it.
    float matrix[4] = {};
    REQUIRE(material.Vec4Of("AlbedoMapTexMat", matrix));
    REQUIRE(matrix[0] == 1.0f);
    REQUIRE(matrix[2] == 0.0f);
}

TEST_CASE("EMAT: malformed input is refused, not half-read", "[asset][material][emat][dz-002]")
{
    const EmatMaterial noBrace = ParseEmat("MatWaterPool {\n Color 1 1 1 1\n");
    REQUIRE_FALSE(noBrace.valid());
    REQUIRE(noBrace.error.find("closing brace") != std::string::npos);

    const EmatMaterial noHeader = ParseEmat("Color 1 1 1 1\n");
    REQUIRE_FALSE(noHeader.valid());

    const EmatMaterial empty = ParseEmat("");
    REQUIRE_FALSE(empty.valid());
    REQUIRE(empty.error.find("empty") != std::string::npos);
}

TEST_CASE("EMAT: the sibling path is derived from the rvmat", "[asset][material][emat][dz-002]")
{
    REQUIRE(EmatPathForRvmat("dz\\water_bliss\\river\\data\\enoch_river.rvmat") ==
            "dz\\water_bliss\\river\\data\\enoch_river.emat");
    REQUIRE(EmatPathForRvmat("noextension") == "noextension.emat");
}

// --- ARF-001: the header forms Reforger uses and DayZ does not ---

TEST_CASE("EMAT: a class may declare its own path", "[asset][material][emat][arf-001]")
{
    // `ClassName "{GUID}ownPath" {` -- 44 of Reforger's 10,312 materials. The old
    // reader searched for the FIRST '{', which here opens the GUID rather than the
    // body, so the class name came out as `MatPBRBasic "`.
    const EmatMaterial material = ParseEmat("MatPBRBasic \"{BA3FFD9519D35F4A}Assets/Props/S2/S2_Bolt/Data/S2_Bolt.emat\" {\n"
                                            " BCRMap \"{3AA30B907536695F}Assets/Props/S2/S2_Bolt/Data/S2_Bolt_BCR.edds\"\n"
                                            "}\n");
    REQUIRE(material.valid());
    REQUIRE(material.className == "MatPBRBasic");
    REQUIRE(material.declaredPath == "Assets/Props/S2/S2_Bolt/Data/S2_Bolt.emat");
    REQUIRE(material.parentPath.empty());
    REQUIRE(material.TextureOf("BCRMap") == "Assets/Props/S2/S2_Bolt/Data/S2_Bolt_BCR.edds");
}

TEST_CASE("EMAT: a class may derive from a parent", "[asset][material][emat][arf-001]")
{
    // `ClassName : "{GUID}parent" {` -- 1,891 of 10,312 (18.3%), 547 distinct
    // parents. Dropping the parent is silent: the child keeps its own maps and
    // loses the whole shading setup.
    const EmatMaterial material =
        ParseEmat("MatPBRSkinProfile : \"{3D8DD71AB39D2267}Assets/_BaseMaterials/Characters_MatPBRSkinProfile.emat\" {\n"
                  " Color 0.738 0.738 0.738 1\n"
                  "}\n");
    REQUIRE(material.valid());
    REQUIRE(material.className == "MatPBRSkinProfile");
    REQUIRE(material.parentPath == "Assets/_BaseMaterials/Characters_MatPBRSkinProfile.emat");
    REQUIRE(material.parentGuid == "3D8DD71AB39D2267");
}

TEST_CASE("EMAT: own path and parent may both appear", "[asset][material][emat][arf-001]")
{
    const EmatMaterial material =
        ParseEmat("MatPBRBasic \"{9D0D26B26B7BC3D8}Assets/Characters/HeadGear/Helmet_DH132/Data/Helmet_DH132_01.emat\""
                  " : \"{5CDDE2709BA9A5EE}Assets/_BaseMaterials/Characters_MatPBRBasic.emat\" {\n"
                  " AllowUserAlphaBias 1\n"
                  "}\n");
    REQUIRE(material.valid());
    REQUIRE(material.className == "MatPBRBasic");
    REQUIRE(material.declaredPath == "Assets/Characters/HeadGear/Helmet_DH132/Data/Helmet_DH132_01.emat");
    REQUIRE(material.parentPath == "Assets/_BaseMaterials/Characters_MatPBRBasic.emat");
}

TEST_CASE("EMAT: inheritance merges the parent under the child", "[asset][material][emat][arf-001]")
{
    EmatMaterial child = ParseEmat("MatPBRBasic : \"{AAAA}base.emat\" {\n"
                                   " Color 0.5 0.5 0.5 1\n"
                                   " BCRMap \"{BBBB}child_BCR.edds\"\n"
                                   "}\n");
    REQUIRE(child.valid());

    auto load = [](const std::string& path, std::string& text)
    {
        if (path != "base.emat")
            return false;
        text = "MatPBRBasic {\n"
               " Color 1 1 1 1\n"
               " BlendMode AlphaBlend\n"
               " NMOMap \"{CCCC}base_NMO.edds\"\n"
               "}\n";
        return true;
    };
    REQUIRE(Poseidon::Asset::Material::ResolveEmatInheritance(child, load));

    // The child wins where it speaks...
    REQUIRE(child.TextureOf("BCRMap") == "child_BCR.edds");
    float colour[4] = {};
    REQUIRE(child.Vec4Of("Color", colour));
    REQUIRE(colour[0] == 0.5f);
    // ...and inherits everything it does not, which is the whole point: without
    // this, BlendMode and the normal map vanish with no error anywhere.
    REQUIRE(child.WordOf("BlendMode") == "AlphaBlend");
    REQUIRE(child.TextureOf("NMOMap") == "base_NMO.edds");
    REQUIRE(child.parentPath.empty()); // fully resolved
}

TEST_CASE("EMAT: an unresolvable or cyclic parent leaves the child untouched", "[asset][material][emat][arf-001]")
{
    // Half-merging is worse than not merging: it looks resolved.
    EmatMaterial missing = ParseEmat("MatPBRBasic : \"{AAAA}nowhere.emat\" {\n Color 1 1 1 1\n}\n");
    auto never = [](const std::string&, std::string&) { return false; };
    REQUIRE_FALSE(Poseidon::Asset::Material::ResolveEmatInheritance(missing, never));
    REQUIRE(missing.parentPath == "nowhere.emat");

    EmatMaterial cyclic = ParseEmat("MatPBRBasic : \"{AAAA}a.emat\" {\n Color 1 1 1 1\n}\n");
    auto loop = [](const std::string& path, std::string& text)
    {
        text = path == "a.emat" ? "MatPBRBasic : \"{BBBB}b.emat\" {\n}\n" : "MatPBRBasic : \"{AAAA}a.emat\" {\n}\n";
        return true;
    };
    REQUIRE_FALSE(Poseidon::Asset::Material::ResolveEmatInheritance(cyclic, loop));
}

// ---------------------------------------------------------------------------
// The writer. The converter deploys a resolved `.emat` for the engine to read back
// (XobCommand's TextureBaker::WriteEmat -> LoadTranslatedMaterial), and this is the
// only place the two halves meet outside a full re-export.
// ---------------------------------------------------------------------------

TEST_CASE("EMAT: WriteEmatText round-trips what ParseEmat read", "[asset][material][emat][arf]")
{
    using Poseidon::Asset::Material::WriteEmatText;
    // Every value kind the corpus has: a 4-vector, a bare word, a texture with its
    // GUID (trailing space verbatim from the jerrycan), a quoted key with a space, a
    // nested block, and a number whose text must not be re-formatted.
    const char* source = "MatPBRMulti {\n"
                         " Color_1 0.103 0.105 0.041 1\n"
                         " Cull none\n"
                         " BCR_1 \"{1837E633B1A92A98}Assets/_SharedData/Metal/ST_MetalPaint_Coated_02_1m_BCR.edds\" \n"
                         " \"Pixel stride_1\" 3\n"
                         " UVTransform_1 MatUVTransform \"{52B49B158EA97A36}\" {\n"
                         "  TilingU 4.9\n"
                         "  TilingV 4\n"
                         " }\n"
                         " Roughness_1 1.118000000001\n"
                         "}";
    const EmatMaterial first = ParseEmat(source);
    REQUIRE(first.valid());
    const std::string written = WriteEmatText(first);
    const EmatMaterial second = ParseEmat(written);
    REQUIRE(second.valid());

    REQUIRE(second.className == first.className);
    REQUIRE(second.parentPath.empty());
    REQUIRE(second.properties.size() == first.properties.size());
    for (size_t i = 0; i < first.properties.size(); ++i)
    {
        const auto& a = first.properties[i];
        const auto& b = second.properties[i];
        REQUIRE(b.name == a.name);
        REQUIRE(b.values.size() == a.values.size());
        for (size_t v = 0; v < a.values.size(); ++v)
        {
            REQUIRE(b.values[v].text == a.values[v].text);
            REQUIRE(b.values[v].guid == a.values[v].guid);
            REQUIRE(b.values[v].isNumber == a.values[v].isNumber);
            REQUIRE(b.values[v].number == a.values[v].number);
        }
    }
    // The things a consumer actually asks for.
    REQUIRE(second.TextureOf("BCR_1") == "Assets/_SharedData/Metal/ST_MetalPaint_Coated_02_1m_BCR.edds");
    REQUIRE(second.WordOf("Cull") == "none");
    REQUIRE(second.FloatOr("Pixel stride_1", 0.0f) == 3.0f);
    float colour[4] = {};
    REQUIRE(second.Vec4Of("Color_1", colour));
    REQUIRE(colour[2] == 0.041f);
    // The number's TEXT survives, not a re-formatted double.
    REQUIRE(written.find("1.118000000001") != std::string::npos);
    // A texture is written with its GUID -- that is what makes it a texture on the way back.
    REQUIRE(written.find("\"{1837E633B1A92A98}Assets/_SharedData") != std::string::npos);
}

TEST_CASE("EMAT: WriteEmatText keeps a word that looks like a number a word", "[asset][material][emat][arf]")
{
    using Poseidon::Asset::Material::WriteEmatText;
    EmatMaterial material;
    material.className = "MatPBRBasic";
    Poseidon::Asset::Material::EmatProperty word;
    word.name = "Tag";
    Poseidon::Asset::Material::EmatValue value;
    value.text = "42"; // a quoted "42" in the source: not a number, by the parser's rule
    word.values.push_back(value);
    material.properties.push_back(word);

    const EmatMaterial back = ParseEmat(WriteEmatText(material));
    REQUIRE(back.valid());
    REQUIRE(back.WordOf("Tag") == "42");
    float asNumber = 0.0f;
    REQUIRE_FALSE(back.FloatOf("Tag", asNumber));
}
