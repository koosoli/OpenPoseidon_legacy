// test_emat_material_adapter.cpp - DZ-002: routing a DayZ .emat into TranslatedMaterial.
//
// The file-reading half (ReadEmatFile / AugmentFromEmatSibling / LoadTranslatedMaterial)
// needs the addon resolver and so is exercised in the engine, not here; what is asserted
// here is the translation itself and the guard that decides when to apply it. The live
// check is the log line a DayZ world now produces:
//
//   Wgpu material: normal map bound for dz\water_bliss\ponds\data\enoch_pond.rvmat
//                  family=CalmWater ( dz\water_bliss\ponds\data\enoch_pond_nohq.edds)

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/Material/EmatMaterialAdapter.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using Poseidon::Asset::Material::BindsNothing;
using Poseidon::Asset::Material::EmatSiblingOf;
using Poseidon::Asset::Material::MaterialSlot;
using Poseidon::Asset::Material::ParseEmat;
using Poseidon::Asset::Material::TranslatedMaterial;
using Poseidon::Asset::Material::TranslatedSlot;
using Poseidon::Asset::Material::TranslateEmat;

namespace
{
// A river material, trimmed to the keys this adapter looks at plus a few it must ignore.
const char* kRiverEmat = "MatWaterPool {\n"
                         " Color 0.0392 0.0549 0.0667 0.3725\n"
                         " ZWrite 0\n"
                         " NormalMap \"{C57E538AD376F42C}DZ/water_bliss/river/data/enoch_river_nohq.edds\"\n"
                         " SunPower 525.937\n"
                         " WaterExtinction 3\n"
                         " EnvironmentMap \"{46C92AE52447906C}Enfusion/Graphics/Textures/Water/env_lake_co.edds\"\n"
                         " WaterStreamMap \"{EF177E7C8B896505}DZ/water_bliss/river/data/enoch_river_flowmap_co.edds\"\n"
                         " StreamSpeedInfl 0.15\n"
                         "}\n";
} // namespace

TEST_CASE("EMAT adapter: a water material yields its normal and environment maps", "[asset][material][emat][dz-002]")
{
    const TranslatedMaterial material =
        TranslateEmat(ParseEmat(kRiverEmat), "dz/water_bliss/river/data/enoch_river.emat", "CalmWater");

    REQUIRE_FALSE(BindsNothing(material));
    REQUIRE(material.schemaKnown);
    // The family stays the rvmat's own claim; the adapter must not invent one.
    REQUIRE(material.shaderFamily == "CalmWater");

    const TranslatedSlot* normal = material.Find(MaterialSlot::NormalMap);
    REQUIRE(normal != nullptr);
    REQUIRE(normal->present);
    REQUIRE(normal->texture.path.canonical().find("enoch_river_nohq.edds") != std::string::npos);
    // An .emat has no stages, so no stage index may be invented for one.
    REQUIRE(normal->sourceStage == -1);

    REQUIRE(material.Find(MaterialSlot::Environment) != nullptr);
}

TEST_CASE("EMAT adapter: Color becomes diffuse, including its opacity", "[asset][material][emat][dz-002]")
{
    const TranslatedMaterial material = TranslateEmat(ParseEmat(kRiverEmat), "origin", "CalmWater");

    REQUIRE(material.diffuse[0] == 0.0392f);
    REQUIRE(material.diffuse[1] == 0.0549f);
    REQUIRE(material.diffuse[2] == 0.0667f);
    // The fourth component is the surface's opacity -- river water is far from opaque,
    // and reading only RGB would render it as solid dark paint.
    REQUIRE(material.diffuse[3] == 0.3725f);
}

TEST_CASE("EMAT adapter: water-shader keys are left alone", "[asset][material][emat][dz-002]")
{
    // SunPower, WaterExtinction and the Stream* trio describe a shading model this
    // engine does not have. `specular` and `emissive` are unused and it would be easy to
    // park them there; doing so would be a claim about their meaning that nothing has
    // measured, so they must stay at their defaults.
    const TranslatedMaterial material = TranslateEmat(ParseEmat(kRiverEmat), "origin", "CalmWater");

    REQUIRE(material.specular[0] == 1.0f);
    REQUIRE(material.specularPower == 1.0f);
    REQUIRE(material.emissive[0] == 0.0f);
    REQUIRE(material.emissive[3] == 1.0f);

    // And the flow map is deliberately NOT a slot: no MaterialSlot means "flow field",
    // and giving it one would put a direction field where a colour is expected.
    for (const auto& slot : material.slots)
        REQUIRE(slot.texture.path.canonical().find("flowmap") == std::string::npos);
}

TEST_CASE("EMAT adapter: an albedo map is taken when the material has one", "[asset][material][emat][dz-002]")
{
    // Rivers and ponds author no albedo; Sakhal's ice lake does.
    const TranslatedMaterial ice =
        TranslateEmat(ParseEmat("MatWaterPool {\n"
                                " AlbedoMap \"{25A9608CDDECF90F}DZ/water_sakhal/data/ice_ca.edds\"\n"
                                " Color 0.3569 0.3843 0.4157 0.1176\n"
                                "}\n"),
                      "origin", "CalmWater");
    const TranslatedSlot* base = ice.Find(MaterialSlot::BaseColour);
    REQUIRE(base != nullptr);
    REQUIRE(base->texture.path.canonical().find("ice_ca.edds") != std::string::npos);

    // A river has no AlbedoMap, so its base colour is synthesised from `Color` instead --
    // see the procedural-colour test below.
    const TranslatedMaterial river = TranslateEmat(ParseEmat(kRiverEmat), "origin", "CalmWater");
    const TranslatedSlot* riverBase = river.Find(MaterialSlot::BaseColour);
    REQUIRE(riverBase != nullptr);
    REQUIRE(riverBase->texture.isProcedural);
}

TEST_CASE("EMAT adapter: a colour-only material becomes a procedural base colour", "[asset][material][emat][dz-002]")
{
    // DayZ water authors no albedo texture at all -- its whole appearance is one
    // constant. Real Virtuality already has a notation for a stage that is a flat
    // colour, and the engine already generates it, so the constant is emitted in that
    // form rather than teaching every backend a new "no texture, use diffuse" path.
    const TranslatedMaterial material = TranslateEmat(ParseEmat(kRiverEmat), "origin", "CalmWater");

    const TranslatedSlot* base = material.Find(MaterialSlot::BaseColour);
    REQUIRE(base != nullptr);
    REQUIRE(base->texture.isProcedural);
    // The generated name must carry the .emat's own colour, alpha included.
    REQUIRE(base->texture.raw.find("color(0.0392,0.0549,0.0667,0.3725") != std::string::npos);
    // A procedural stage is generated from its name, so it has no path to load.
    REQUIRE(base->texture.path.empty());
    // ...and a material holding only one must still count as bound, or the adapter
    // would reject its own output.
    REQUIRE_FALSE(BindsNothing(material));
}

TEST_CASE("EMAT adapter: the guard only fires on a material that bound nothing", "[asset][material][emat][dz-002]")
{
    // This is what keeps the adapter away from every ordinary Arma material: it may only
    // replace a material that has no textures at all, which is the state a CalmWater
    // stub produces and one a real shader family never reaches.
    TranslatedMaterial empty;
    REQUIRE(BindsNothing(empty));

    TranslatedSlot slot;
    slot.slot = MaterialSlot::BaseColour;
    slot.present = true;
    slot.texture = Poseidon::Asset::Material::RvTextureRef::Parse("data\\wall_co.paa");
    TranslatedMaterial bound;
    bound.slots.push_back(slot);
    REQUIRE_FALSE(BindsNothing(bound));

    // A slot that exists but names no texture is still "bound nothing" -- a schema can
    // declare a slot the material never filled.
    TranslatedSlot declaredOnly;
    declaredOnly.slot = MaterialSlot::NormalMap;
    declaredOnly.present = true;
    TranslatedMaterial hollow;
    hollow.slots.push_back(declaredOnly);
    REQUIRE(BindsNothing(hollow));
}

TEST_CASE("EMAT adapter: an .emat with nothing to give is not an improvement", "[asset][material][emat][dz-002]")
{
    // No maps AND no colour: there is nothing to give the surface, so the rvmat's own
    // translation should be left alone rather than replaced by an equally empty one.
    const TranslatedMaterial nothing =
        TranslateEmat(ParseEmat("MatWaterPool {\n WindInfl 0.1\n ZWrite 0\n}\n"), "origin", "CalmWater");
    REQUIRE(BindsNothing(nothing));

    // A material with only a colour is a different case and IS an improvement -- an
    // `enoch_50x50_clear`-style pond authors no maps at all, and its constant is the
    // whole of its appearance.
    const TranslatedMaterial colourOnly =
        TranslateEmat(ParseEmat("MatWaterPool {\n Color 0 0.0588 0.1569 1\n WindInfl 0.1\n}\n"), "origin", "CalmWater");
    REQUIRE_FALSE(BindsNothing(colourOnly));
    REQUIRE(colourOnly.Find(MaterialSlot::BaseColour) != nullptr);
}

TEST_CASE("EMAT adapter: the sibling path swaps the extension", "[asset][material][emat][dz-002]")
{
    REQUIRE(EmatSiblingOf("dz\\water_bliss\\ponds\\data\\enoch_pond.rvmat") ==
            "dz\\water_bliss\\ponds\\data\\enoch_pond.emat");
}

// ---------------------------------------------------------------------------
// Arma Reforger. Everything below is verbatim from the shipped corpus.
//
// Three separate defects meant that every one of Everon's 1,189 materials bound
// nothing at all -- no albedo through the material, no normal, no specular, on any
// object in the world:
//
//   1. The section names an `.emat`, and `LoadTranslatedMaterial` handed it to
//      `ParseRvMaterialFile`, which threw "failed to open RVMAT" every time.
//   2. `AugmentFromEmatSibling` compared `EmatPathForRvmat(p)` with `p` -- for a path
//      that already IS an `.emat` those are the same string -- and read the equality
//      as "no sibling exists", so the only branch that can read an `.emat` never ran.
//   3. `TranslateEmat` read `AlbedoMap`/`NormalMap`/`EnvironmentMap`, which are DayZ's
//      names. Over Reforger's 10,312 `.emat` files, 9,481 name at least one texture
//      and only 223 -- 2.2% -- name any key it was looking for.
// ---------------------------------------------------------------------------

namespace
{
// Assets/Structures/Airport/ControlTower_01/Data/ControlTower_01_Brick_Ruin.emat
const char* kBasicEmat = "MatPBRBasic {\n"
                         " Color 0.947 0.947 0.947 1\n"
                         " MetalnessScale 0\n"
                         " BCRMap \"{2F3B53464E5036CB}Assets/_SharedData/Brick/ST_Brick_detail_BCR.edds\"\n"
                         " NMOMap \"{17E3E8D497276455}Assets/_SharedData/Brick/ST_Brick_detail_NMO.edds\"\n"
                         "}";

// Assets/Items/Fuel/Jerrycan_01/Data/JerryCan_01.emat, trimmed to layers 1 and 2.
// The trailing space after each texture reference is verbatim.
const char* kMultiEmat =
    "MatPBRMulti {\n"
    " MaskMap \"{8FBF820AA7B85CA5}Assets/Items/Fuel/Jerrycan_01/Data/JerryCan_01_GLOBAL_MASK.edds\" \n"
    " GlobalNMOMap \"{86747DE209CE9C85}Assets/Items/Fuel/Jerrycan_01/Data/JerryCan_01_NMO.edds\" \n"
    " Enabled_2 1\n"
    " Color_2 0.07 0.078 0.035 1\n"
    " BCR_2 \"{1837E633B1A92A98}Assets/_SharedData/Metal/ST_MetalPaint_Coated_02_1m_BCR.edds\" \n"
    " NMO_2 \"{839CBC1678749998}Assets/_SharedData/Metal/ST_MetalPaint_Coated_1m_NMO.edds\" \n"
    " UVTransform_2 MatUVTransform \"{52B49B158EA97A36}\" {\n"
    "  TilingU 4.9\n"
    "  TilingV 4\n"
    " }\n"
    " Color_1 0.103 0.105 0.041 1\n"
    " BCR_1 \"{1837E633B1A92A98}Assets/_SharedData/Metal/ST_MetalPaint_Coated_02_1m_BCR.edds\" \n"
    " NMO_1 \"{839CBC1678749998}Assets/_SharedData/Metal/ST_MetalPaint_Coated_1m_NMO.edds\" \n"
    " Roughness_1 1.118\n"
    "}";

// Assets/Vegetation/Tree/Betula_Pendula/Data/t_betula_pendula_2f_polyplane.emat,
// trimmed. The vegetation families spell the normal `NTCMap`, not `NMOMap`.
const char* kCrownEmat =
    "MatPBRTreeCrown : \"{43AE18CB72CB7084}Assets/Vegetation/_Data/crown_base.emat\" {\n"
    " Cull none\n"
    " BCRMap \"{090038B43B18FB8E}Assets/Vegetation/_Polyplanes/polyplane_betula_pendula_BCR.edds\"\n"
    " OpacityMap \"{1C1913DBEE0C77A0}Assets/Vegetation/_Polyplanes/polyplane_betula_pendula_A.edds\"\n"
    " NormalPower 3\n"
    " NTCMap \"{9A6ED533F062788B}Assets/Vegetation/_Polyplanes/polyplane_betula_pendula_NTC.edds\"\n"
    "}";
} // namespace

TEST_CASE("EMAT adapter: a Reforger MatPBRBasic binds its BCR and its NMO", "[asset][material][emat][arf]")
{
    const TranslatedMaterial material =
        TranslateEmat(ParseEmat(kBasicEmat), "assets/structures/airport/controltower_01_brick_ruin.emat", "");

    REQUIRE_FALSE(BindsNothing(material));

    const TranslatedSlot* base = material.Find(MaterialSlot::BaseColour);
    REQUIRE(base != nullptr);
    REQUIRE(base->texture.path.canonical().find("st_brick_detail_bcr.edds") != std::string::npos);

    // The whole point: 2,402 materials name a normal this way and NONE of them bound
    // one before, because the key being read was DayZ's `NormalMap`.
    const TranslatedSlot* normal = material.Find(MaterialSlot::NormalMap);
    REQUIRE(normal != nullptr);
    REQUIRE(normal->texture.path.canonical().find("st_brick_detail_nmo.edds") != std::string::npos);

    // With no rvmat to claim a family, the class name is the honest answer. Everon
    // logged `family=` on every material before this.
    REQUIRE(material.shaderFamily == "MatPBRBasic");
}

TEST_CASE("EMAT adapter: MatPBRMulti's layer tiles never reach the object's slots", "[asset][material][emat][arf]")
{
    const TranslatedMaterial material =
        TranslateEmat(ParseEmat(kMultiEmat), "assets/items/fuel/jerrycan_01/data/jerrycan_01.emat", "");

    // `GlobalNMOMap` is the jerrycan's OWN normal, in the jerrycan's UV layout.
    const TranslatedSlot* normal = material.Find(MaterialSlot::NormalMap);
    REQUIRE(normal != nullptr);
    REQUIRE(normal->texture.path.canonical().find("jerrycan_01_nmo.edds") != std::string::npos);

    // `NMO_1` is one square metre of shared painted metal, tiled 4x4 by its own
    // transform and used by hundreds of unrelated objects. Bound as the object's normal
    // map it would light every surface wrongly -- worse than the nothing it replaces --
    // so it must land in a layer slot and nowhere else.
    REQUIRE(normal->texture.path.canonical().find("st_metalpaint") == std::string::npos);
    const TranslatedSlot* layerNormal = material.Find(MaterialSlot::LayerNormal1);
    REQUIRE(layerNormal != nullptr);
    REQUIRE(layerNormal->texture.path.canonical().find("st_metalpaint_coated_1m_nmo.edds") != std::string::npos);

    // The same argument applies to BCR_1: this material has no albedo of its own.
    REQUIRE(material.Find(MaterialSlot::BaseColour) == nullptr);
    REQUIRE(material.Find(MaterialSlot::LayerColour1) != nullptr);

    // The blend control is recorded even though nothing samples it yet.
    const TranslatedSlot* mask = material.Find(MaterialSlot::Mask);
    REQUIRE(mask != nullptr);
    REQUIRE(mask->texture.path.canonical().find("jerrycan_01_global_mask.edds") != std::string::npos);
}

TEST_CASE("EMAT adapter: the vegetation families' NTCMap is a normal", "[asset][material][emat][arf]")
{
    const TranslatedMaterial material = TranslateEmat(ParseEmat(kCrownEmat), "t_betula_pendula_2f_polyplane.emat", "");

    const TranslatedSlot* normal = material.Find(MaterialSlot::NormalMap);
    REQUIRE(normal != nullptr);
    REQUIRE(normal->texture.path.canonical().find("polyplane_betula_pendula_ntc.edds") != std::string::npos);
    REQUIRE(material.Find(MaterialSlot::BaseColour) != nullptr);
    REQUIRE(material.shaderFamily == "MatPBRTreeCrown");
}

TEST_CASE("EMAT adapter: an .emat path is recognised as one", "[asset][material][emat][arf]")
{
    using Poseidon::Asset::Material::IsEmatPath;
    REQUIRE(IsEmatPath("assets/props/x.emat"));
    REQUIRE(IsEmatPath("ASSETS/PROPS/X.EMAT")); // Reforger's casing is not stable
    REQUIRE_FALSE(IsEmatPath("dz\\water_bliss\\ponds\\data\\enoch_pond.rvmat"));
    REQUIRE_FALSE(IsEmatPath(".emat")); // an extension with no stem is not a path
    REQUIRE_FALSE(IsEmatPath(""));

    // Defect 2 stated as the equality that caused it. `EmatSiblingOf` is the identity on
    // an `.emat`, and the old guard read that as "there is no sibling here".
    REQUIRE(EmatSiblingOf("assets/props/x.emat") == "assets/props/x.emat");
}

TEST_CASE("EMAT adapter: the emitted Super RVMAT binds the normal it names", "[asset][material][emat][arf]")
{
    // The converter writes this file and the engine reads it back, and the two halves
    // are only ever exercised together in a full re-export -- which is slow enough that
    // a format mistake would be found by a rendered frame rather than by a test. So the
    // emitter's own output goes straight through the real parser here.
    //
    // Nothing on Everon can bind a normal map without this file existing: a normal has
    // no face-texture slot to arrive through, only a material stage.
    const std::string text = Poseidon::Asset::Material::MakeSuperRvmatText(
        "Assets/Structures/Airport/ControlTower_01/Data/ControlTower_01_Ext_01.emat",
        "reforger\\everon\\textures\\assets\\_shareddata\\concrete\\st_concrete_01_nohq.paa");

    const auto path = std::filesystem::temp_directory_path() / "poseidon_super_rvmat_roundtrip.rvmat";
    {
        std::ofstream file(path, std::ios::trunc);
        REQUIRE(file);
        file << text;
    }

    const TranslatedMaterial material = Poseidon::Asset::Material::LoadTranslatedMaterial(path.string());
    std::filesystem::remove(path);

    // The family has to be one the schema knows, or every stage is dropped unmapped.
    REQUIRE(material.shaderFamily == "Super");
    REQUIRE(material.schemaKnown);

    const TranslatedSlot* normal = material.Find(MaterialSlot::NormalMap);
    REQUIRE(normal != nullptr);
    REQUIRE(normal->sourceStage == 1); // Super's Stage1, and no other stage will do
    REQUIRE(normal->texture.path.canonical().find("st_concrete_01_nohq.paa") != std::string::npos);

    // Stage0 stays out: the albedo comes from the MLOD face texture, which is also the
    // only thing `ClassifyGpuSection` reads a cutout decision from.
    REQUIRE(material.Find(MaterialSlot::BaseColour) == nullptr);
}

TEST_CASE("EMAT adapter: a DayZ material still reads the way it did", "[asset][material][emat][dz-002]")
{
    // The Reforger keys are additions, not replacements: DayZ's water must be unaffected
    // by them, and its `NormalMap` must not lose to a Reforger key that is absent.
    const TranslatedMaterial river = TranslateEmat(ParseEmat(kRiverEmat), "origin", "CalmWater");
    const TranslatedSlot* normal = river.Find(MaterialSlot::NormalMap);
    REQUIRE(normal != nullptr);
    REQUIRE(normal->texture.path.canonical().find("enoch_river_nohq.edds") != std::string::npos);
    REQUIRE(river.shaderFamily == "CalmWater");
    REQUIRE(river.Find(MaterialSlot::Environment) != nullptr);
}
