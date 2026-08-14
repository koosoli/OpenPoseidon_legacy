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
