// test_emat_material_adapter.cpp - DZ-002: routing a DayZ .emat into TranslatedMaterial.
//
// The file-reading half (ReadEmatFile / AugmentFromEmatSibling / LoadTranslatedMaterial)
// needs the addon resolver and so is exercised in the engine, not here; what is asserted
// here is the translation itself and the guard that decides when to apply it. The live
// check is the log line a DayZ world now produces:
//
//   Wgpu material: normal map bound for dz\water_bliss\ponds\data\enoch_pond.rvmat
//                  family=CalmWater ( dz\water_bliss\ponds\data\enoch_pond_nohq.edds)

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <Poseidon/Asset/Formats/Material/EmatMaterialAdapter.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using Poseidon::Asset::Material::BindsNothing;
using Poseidon::Asset::Material::EmatSiblingOf;
using Poseidon::Asset::Material::EmatPbrProvenanceEnabled;
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
    REQUIRE(material.normalPower == 1.0f);
    REQUIRE(material.emissive[0] == 0.0f);
    REQUIRE(material.emissive[3] == 1.0f);

    // And the flow map is deliberately NOT a slot: no MaterialSlot means "flow field",
    // and giving it one would put a direction field where a colour is expected.
    for (const auto& slot : material.slots)
        REQUIRE(slot.texture.path.canonical().find("flowmap") == std::string::npos);
}

TEST_CASE("EMAT adapter: NormalPower carries the authored normal intensity", "[asset][material][emat]")
{
    // A crown fragment: 2.5x leaf relief. Absent keys stay the 1.0 no-op; a
    // negative value clamps at parse so downstream code never repeats the guard.
    const char* crown = "MatPBRTreeCrown {\n"
                        " NormalPower 2.5\n"
                        " BCRMap \"{00}tilia_bcr.edds\"\n"
                        " NTCMap \"{00}tilia_ntc.edds\"\n"
                        "}\n";
    REQUIRE(TranslateEmat(ParseEmat(crown), "origin", "").normalPower == 2.5f);
    REQUIRE(TranslateEmat(ParseEmat(kRiverEmat), "origin", "CalmWater").normalPower == 1.0f);

    const char* negative = "MatPBRBasic {\n"
                           " NormalPower -2\n"
                           " BCRMap \"{00}x.edds\"\n"
                           "}\n";
    REQUIRE(TranslateEmat(ParseEmat(negative), "origin", "").normalPower == 0.0f);
}

TEST_CASE("EMAT adapter: crown AO volume parses whole or not at all", "[asset][material][emat]")
{
    const char* crown = "MatPBRTreeCrown {\n"
                        " GeometryAOCenter 0 7 0\n"
                        " GeometryAOHeight 7.863\n"
                        " GeometryAOWidth 2.308\n"
                        " GeometryAOIntensity 1\n"
                        " BCRMap \"{00}crown.edds\"\n"
                        "}\n";
    const TranslatedMaterial full = TranslateEmat(ParseEmat(crown), "origin", "");
    REQUIRE(full.crownAoCenter[1] == 7.0f);
    REQUIRE(full.crownAoHeight == 7.863f);
    REQUIRE(full.crownAoWidth == 2.308f);
    REQUIRE(full.crownAoIntensity == 1.0f);

    // Missing intensity: the volume cannot evaluate, so nothing is carried.
    const char* partial = "MatPBRTreeCrown {\n"
                          " GeometryAOCenter 0 7 0\n"
                          " GeometryAOHeight 7.863\n"
                          " GeometryAOWidth 2.308\n"
                          " BCRMap \"{00}crown.edds\"\n"
                          "}\n";
    REQUIRE(TranslateEmat(ParseEmat(partial), "origin", "").crownAoIntensity == 0.0f);
    // A legacy-style material names none of it.
    REQUIRE(TranslateEmat(ParseEmat(kRiverEmat), "origin", "CalmWater").crownAoIntensity == 0.0f);
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

TEST_CASE("EMAT adapter: a Reforger coarse-LOD MatPBRBasic is bound from its Color alone",
          "[asset][material][emat][mat-054]")
{
    // MAT-054. Everon's white blocks at distance are not a water case and not a DayZ
    // case, and they are the reason this contract has to be pinned for MatPBRBasic and
    // not only for MatWaterPool: Reforger's BAKED-LOD materials -- the `*_MLOD`,
    // `*_lod3` and `*_Color_palette` files a model's coarse LODs name -- carry a single
    // flat `Color` and NO map of any kind. Verbatim from
    // `Assets/Structures/Walls/Crashbarrier/data/CrashBarrier_MLOD.emat` (data005.pak);
    // 12 of the 40 materials in the measured untextured set are this shape, and every
    // one of their colours is dark (0.016 to 0.32), so the 1.0 white a section falls to
    // when nothing binds is the largest error the format can produce.
    //
    // The adapter has always handled this -- the synthesis is keyed on `Color` and not
    // on a class -- and that is exactly why it needs a test naming this family: the fix
    // for MAT-054 lives in the converter, which must deploy such an `.emat` even though
    // it bakes no `.paa` for it, and it may do so ONLY while this holds.
    const TranslatedMaterial mlod = TranslateEmat(ParseEmat("MatPBRBasic {\n"
                                                            " Color 0.168 0.185 0.188 1\n"
                                                            " Cull none\n"
                                                            " SurfaceProperties "
                                                            "\"{CE9253778DD8FBDE}Common/Materials/Game/metal.gamemat\"\n"
                                                            " RoughnessScale 0.48\n"
                                                            " MetalnessScale 0.838\n"
                                                            "}\n"),
                                                 "Assets/Structures/Walls/Crashbarrier/data/CrashBarrier_MLOD.emat",
                                                 "");

    const TranslatedSlot* base = mlod.Find(MaterialSlot::BaseColour);
    REQUIRE(base != nullptr);
    REQUIRE(base->texture.isProcedural);
    REQUIRE(base->texture.raw.find("color(0.168,0.185,0.188,1") != std::string::npos);
    // The whole point: a material of this shape must NOT read as binding nothing, or
    // the section reaches the GPU on bindless slot 0 and draws white.
    REQUIRE_FALSE(BindsNothing(mlod));
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

TEST_CASE("EMAT PBR provenance: opaque WaterTank and Granite BCR alpha is roughness, not coverage",
          "[asset][material][emat][arf][pbr-provenance]")
{
    // Verbatim source keys from the installed Reforger archives. WaterTank's
    // BC7 top mip has non-binary alpha (mean 220.4); Granite's layered BCR
    // tiles likewise carry data alpha, not cutout coverage.
    const auto tank = TranslateEmat(
        ParseEmat("MatPBRBasic {\n Cull none\n MetalnessScale 0\n AOScale 0.5\n"
                  " BCRMap \"{F31E1072EF532AE9}Assets/Props/Agricultural/WaterTank_01/Data/WaterTank_01_MLOD_BCR.edds\"\n}\n"),
        "Assets/Props/Agricultural/WaterTank_01/Data/WaterTank_01_MLOD.emat", "");
    if (!EmatPbrProvenanceEnabled())
    {
        REQUIRE_FALSE(tank.ematPbr);
        return;
    }
    REQUIRE(tank.ematPbr);
    REQUIRE(tank.ematPbr->bcrAlphaIsRoughness[0]);
    REQUIRE_FALSE(tank.ematPbr->bcrAlphaIsRoughness[1]);
    REQUIRE(tank.ematPbr->metalnessScale.present);
    REQUIRE(tank.ematPbr->metalnessScale.value == 0.0f);
    REQUIRE_FALSE(tank.ematPbr->roughnessScale.present);

    // Real GraniteBoulder_01 MatPBRMulti uses distinct authored BCR tiles and
    // roughness/metalness scalars; a zero metalness is still an authored value.
    const auto granite = TranslateEmat(
        ParseEmat("MatPBRMulti {\n RoughnessScale 0.85\n"
                  " BCR_1 \"{C1B825901DFF5ACA}Assets/Rocks/Granite/Data/Granite_08_BCR.edds\"\n"
                  " Metalness_1 0\n Enabled_2 1\n"
                  " BCR_2 \"{808840ED6C676545}Assets/Rocks/Granite/Data/Granite_04_BCR.edds\"\n"
                  " Metalness_2 0\n}\n"),
        "Assets/Rocks/Granite/Data/GraniteBoulder_01.emat", "");
    REQUIRE(granite.ematPbr->bcrAlphaIsRoughness[0]);
    REQUIRE(granite.ematPbr->bcrAlphaIsRoughness[1]);
    REQUIRE_FALSE(granite.ematPbr->bcrAlphaIsRoughness[2]);
    REQUIRE(granite.ematPbr->roughnessScale.present);
    REQUIRE(granite.ematPbr->roughnessScale.value == 0.85f);
    REQUIRE(granite.ematPbr->metalness[0].present);
    REQUIRE(granite.ematPbr->metalness[0].value == 0.0f);
    REQUIRE(granite.ematPbr->metalness[1].present);
    REQUIRE_FALSE(granite.ematPbr->metalness[2].present);

    const auto jerrycan = TranslateEmat(ParseEmat(kMultiEmat), "Assets/Items/Fuel/Jerrycan_01/Data/JerryCan_01.emat", "");
    REQUIRE(jerrycan.ematPbr->roughness[0].present);
    REQUIRE(jerrycan.ematPbr->roughness[0].value == 1.118f);
    REQUIRE_FALSE(jerrycan.ematPbr->roughness[1].present);
}

TEST_CASE("EMAT PBR provenance: Alnus OpacityMap and deployed PAA never claim BCR roughness",
          "[asset][material][emat][arf][pbr-provenance]")
{
    // The native face becomes enfa|OpacityMap|BCRMap. Its GPU alpha is
    // coverage, even though the original BCR source carries roughness.
    const auto alnus = TranslateEmat(
        ParseEmat("MatPBRTreeCrown {\n"
                  " BCRMap \"{CFF8762C3EB0A8BC}Assets/Vegetation/_Polyplanes/polyplane_alnus_glutinosa_BCR.edds\"\n"
                  " OpacityMap \"{CAC2E08429009AF9}Assets/Vegetation/_Polyplanes/polyplane_alnus_glutinosa_A.edds\"\n}\n"),
        "Assets/Vegetation/Tree/Alnus_Glutinosa/Data/t_alnus_glutinosa_2f_polyplane.emat", "");
    if (!EmatPbrProvenanceEnabled())
    {
        REQUIRE_FALSE(alnus.ematPbr);
        return;
    }
    REQUIRE(alnus.ematPbr);
    for (bool roughness : alnus.ematPbr->bcrAlphaIsRoughness)
        REQUIRE_FALSE(roughness);

    // Converted material files can keep the BCRMap key while pointing at a
    // baked PAA, whose alpha has different semantics.
    const auto deployed = TranslateEmat(
        ParseEmat("MatPBRBasic {\n BCRMap \"reforger/everon/textures/watertank_01_mlod_ca.paa\"\n}\n"),
        "reforger/everon/textures/watertank_01_mlod.emat", "");
    REQUIRE_FALSE(deployed.ematPbr->bcrAlphaIsRoughness[0]);

    const auto opaqueCutout = TranslateEmat(
        ParseEmat("MatPBRBasic {\n AlphaTest 1\n BCRMap \"Assets/Props/Flag_BCR.edds\"\n}\n"),
        "Assets/Props/Flag.emat", "");
    REQUIRE_FALSE(opaqueCutout.ematPbr->bcrAlphaIsRoughness[0]);
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

    // `BCR_1` IS the base, and leaving BaseColour empty was the bug: Enfusion's mask
    // weights layers 2, 3 and 4 and layer 1 takes the remainder (measured on
    // WaterTower_02's global mask -- channel means 0.127 / 0.299 / 0.238, summing to
    // 0.664). That is also why layer 1 alone has no `Enabled_1`. Binding BCR_1 to
    // LayerColour1 shifted every layer down one, so the mask's red channel weighted the
    // base against itself and layer 4 was never bound at all.
    const TranslatedSlot* base = material.Find(MaterialSlot::BaseColour);
    REQUIRE(base != nullptr);
    REQUIRE(base->texture.path.canonical().find("st_metalpaint_coated_02_1m_bcr.edds") != std::string::npos);
    const TranslatedSlot* layer1 = material.Find(MaterialSlot::LayerColour1);
    REQUIRE(layer1 != nullptr);

    // Each layer at its own scale. Without the transform a 4.9x tile and a 1x tile
    // collapse onto one frame, and that scale difference is what the surface reads as.
    REQUIRE(layer1->uvTransform.present);
    REQUIRE(layer1->uvTransform.aside[0] == Catch::Approx(4.9f));
    REQUIRE(layer1->uvTransform.up[1] == Catch::Approx(4.0f));

    // The blend control, and the frame it is sampled in. `uvSource` is deliberately
    // EMPTY: `UVSrcGlobalMaps "UV set 2"` numbers channels in the SOURCE .xob, which the
    // converter does not preserve -- it writes the object's own unwrap as set 0, and that
    // is where the exported mask lives. Reading it as the engine's `tex1` drew broad
    // diagonal ribbons across church_01's walls and roof.
    const TranslatedSlot* mask = material.Find(MaterialSlot::Mask);
    REQUIRE(mask != nullptr);
    REQUIRE(mask->texture.path.canonical().find("jerrycan_01_global_mask.edds") != std::string::npos);
    REQUIRE(mask->uvSource.empty());
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

// ---------------------------------------------------------------------------
// Deploying the `.emat` -- why "none opens" on Everon, and the two halves of the fix.
//
// Measured 2026-08-16 (`.tmp-shadow/reforger/owner-trees.log`): 737 distinct
// `Wgpu material: no albedo for Assets/...emat (...) family=` lines, family EMPTY --
// so TranslateEmat never ran, because `Assets/Vegetation/...emat` names a file
// inside a `.pak` and nothing under the game root is called `Assets\`. The converter
// now deploys each material it baked under its texture prefix at the same relative
// path (`MakeDeployedEmat` + `WriteEmatText`, spelled by `VirtualNameFor`), and the
// engine looks there when the raw path fails (`EmatDeployRoots`). Either half alone
// is a file nobody reads or a lookup that finds nothing.
// ---------------------------------------------------------------------------

namespace
{
using Poseidon::Asset::Material::DeployedEmatPath;
using Poseidon::Asset::Material::EmatMaterial;
using Poseidon::Asset::Material::EmatUsesLeafCards;
using Poseidon::Asset::Material::LoadTranslatedMaterial;
using Poseidon::Asset::Material::MakeDeployedEmat;
using Poseidon::Asset::Material::ParseEmatDeployRoots;
using Poseidon::Asset::Material::ReadEmatFile;
using Poseidon::Asset::Material::SetEmatDeployRoots;
using Poseidon::Asset::Material::WriteEmatText;

// Assets/Vegetation/Tree/Alnus_Glutinosa/Data/t_alnus_glutinosa_2f_polyplane.emat,
// verbatim, minus the parent reference (crown_base only adds wetness/porosity keys).
// This is the tree the owner reported as "big flat leaf polygons".
const char* kAlderCrownEmat =
    "MatPBRTreeCrown {\n"
    " SpecularIBL 0.433 0.456 0.346 1\n"
    " SpecularMul 0.7\n"
    " GlobalAOPower 0.5\n"
    " BCRMap \"{CFF8762C3EB0A8BC}Assets/Vegetation/_Polyplanes/polyplane_alnus_glutinosa_BCR.edds\"\n"
    " OpacityMap \"{CAC2E08429009AF9}Assets/Vegetation/_Polyplanes/polyplane_alnus_glutinosa_A.edds\"\n"
    " NormalPower 2.5\n"
    " NTCMap \"{5C969BABF5CA2BB9}Assets/Vegetation/_Polyplanes/polyplane_alnus_glutinosa_NTC.edds\"\n"
    " WindSmoothness 1\n"
    " EnableColorization Full\n"
    " WorldColorizeMap \"{933013A75A6BD5E7}Assets/Vegetation/Tree/Data/vege_color_noise_02_COLOR.edds\"\n"
    " ColorizeMapBlendMask "
    "\"{528252C9249492FF}Assets/Vegetation/_Polyplanes/polyplane_alnus_glutinosa_aut_MASK.edds\"\n"
    " SSSColor 0.471 0.471 0.471 1\n"
    "}\n";

const char* kAlderColourPaa =
    "reforger\\everon\\textures\\assets\\vegetation\\_polyplanes\\polyplane_alnus_glutinosa_bcr_ca.paa";
const char* kAlderNormalPaa =
    "reforger\\everon\\textures\\assets\\vegetation\\_polyplanes\\polyplane_alnus_glutinosa_nohq.paa";

struct DeployRootsScope
{
    std::vector<std::string> saved = Poseidon::Asset::Material::EmatDeployRoots();
    ~DeployRootsScope() { SetEmatDeployRoots(saved); }
};
} // namespace

TEST_CASE("EMAT deploy: the alder crown deploys as a cutout that binds its baked albedo and normal",
          "[asset][material][emat][arf]")
{
    const EmatMaterial source = ParseEmat(kAlderCrownEmat);
    REQUIRE(source.valid());
    const EmatMaterial deployed =
        MakeDeployedEmat(source, kAlderColourPaa, /*opacityFolded=*/true, "NTCMap", kAlderNormalPaa);

    // Through the real writer and the real parser, as the game will see it.
    const EmatMaterial back = ParseEmat(WriteEmatText(deployed));
    REQUIRE(back.valid());
    REQUIRE(back.className == "MatPBRTreeCrown");
    REQUIRE(back.parentPath.empty()); // stands alone; no reference into a .pak

    // Textures: exactly the two files the converter wrote, nothing it did not.
    REQUIRE(back.TextureOf("BCRMap") == kAlderColourPaa);
    REQUIRE(back.TextureOf("NTCMap") == kAlderNormalPaa);
    REQUIRE_FALSE(back.Has("OpacityMap"));       // folded into the _ca alpha
    REQUIRE_FALSE(back.Has("WorldColorizeMap")); // an .edds inside a .pak: not deployed
    REQUIRE_FALSE(back.Has("ColorizeMapBlendMask"));
    // ...and it says so: the folded opacity is recorded as Enfusion's own alpha-test key.
    REQUIRE(back.FloatOr("AlphaTest", 0.0f) == 1.0f);
    // Non-texture keys survive verbatim.
    REQUIRE(back.FloatOr("SpecularMul", 0.0f) == 0.7f);
    REQUIRE(back.WordOf("EnableColorization") == "Full");

    // What the engine then binds.
    const TranslatedMaterial material = TranslateEmat(back, "deployed", "");
    REQUIRE(material.shaderFamily == "MatPBRTreeCrown");
    const TranslatedSlot* base = material.Find(MaterialSlot::BaseColour);
    REQUIRE(base != nullptr);
    REQUIRE(base->texture.path.canonical().find("polyplane_alnus_glutinosa_bcr_ca.paa") != std::string::npos);
    const TranslatedSlot* normal = material.Find(MaterialSlot::NormalMap);
    REQUIRE(normal != nullptr);
    REQUIRE(normal->texture.path.canonical().find("polyplane_alnus_glutinosa_nohq.paa") != std::string::npos);
    REQUIRE(material.Find(MaterialSlot::Mask) == nullptr);

    // And it is a leaf card -- the answer GpuSectionMaterialUsesLeafCards gives TreeAdv.
    REQUIRE(EmatUsesLeafCards(back));
}

TEST_CASE("EMAT deploy: no normal baked means no normal named", "[asset][material][emat][arf]")
{
    // The 21:23 re-export ran without --normals: the deployed file must not name a
    // `_nohq.paa` that does not exist. A material that names a missing file is worse
    // than one that names nothing -- it fails to load instead of falling back.
    const EmatMaterial deployed = MakeDeployedEmat(ParseEmat(kAlderCrownEmat), kAlderColourPaa, true, "NTCMap", "");
    const EmatMaterial back = ParseEmat(WriteEmatText(deployed));
    REQUIRE(back.TextureOf("BCRMap") == kAlderColourPaa);
    REQUIRE_FALSE(back.Has("NTCMap"));
    REQUIRE(TranslateEmat(back, "deployed", "").Find(MaterialSlot::NormalMap) == nullptr);
}

TEST_CASE("EMAT deploy: a MatPBRMulti's tinted tile becomes its BCRMap and its layers stay in the pak",
          "[asset][material][emat][arf]")
{
    // The converter bakes BCR_1 rescaled to Color_1 as the object's colour. Deployed
    // under BCR_1 it would land in LayerColour1 and nothing would sample it; under
    // BCRMap it is the base colour. MaskMap and NMO_N are library files inside the
    // .pak and must not be named -- a MaskMap that resolves with no layers behind it
    // puts the section on the layered path with three empty layers.
    const char* colour = "reforger\\everon\\textures\\assets\\_shareddata\\metal\\st_metalpaint_coated_02_1m_bcr.paa";
    const char* normal = "reforger\\everon\\textures\\assets\\items\\fuel\\jerrycan_01\\data\\jerrycan_01_nohq.paa";
    const EmatMaterial back =
        ParseEmat(WriteEmatText(MakeDeployedEmat(ParseEmat(kMultiEmat), colour, false, "GlobalNMOMap", normal)));
    REQUIRE(back.valid());
    REQUIRE(back.TextureOf("BCRMap") == colour);
    REQUIRE(back.TextureOf("GlobalNMOMap") == normal);
    REQUIRE_FALSE(back.Has("MaskMap"));
    REQUIRE_FALSE(back.Has("BCR_1"));
    REQUIRE_FALSE(back.Has("NMO_1"));
    REQUIRE_FALSE(back.Has("AlphaTest")); // nothing was folded; an opaque jerrycan stays opaque
    // Non-texture layer keys stay: Color_1, Roughness_1, the UV transform block.
    REQUIRE(back.Has("Color_1"));
    REQUIRE(back.Has("UVTransform_2"));

    const TranslatedMaterial material = TranslateEmat(back, "deployed", "");
    REQUIRE(material.Find(MaterialSlot::BaseColour) != nullptr);
    REQUIRE(material.Find(MaterialSlot::NormalMap) != nullptr);
    REQUIRE(material.Find(MaterialSlot::Mask) == nullptr);
    REQUIRE(material.Find(MaterialSlot::LayerColour1) == nullptr);
    REQUIRE_FALSE(EmatUsesLeafCards(back));
}

TEST_CASE("EMAT deploy: what counts as a leaf card, and BCR alpha does not", "[asset][material][emat][arf]")
{
    // MatPBRTreeCrown, Grass, an OpacityMap, or AlphaTest > 0. NOT the class of the
    // trunk, and NOT a MatPBRBasic that has only a BCRMap -- its alpha is roughness
    // (measured 99.8% partial on WaterTank_01_MLOD_BCR, 100% on a grass polyplane).
    REQUIRE(EmatUsesLeafCards(ParseEmat("MatPBRTreeCrown {\n}\n")));
    REQUIRE(EmatUsesLeafCards(ParseEmat("Grass {\n BCRMap \"{1}a_BCR.edds\"\n}\n")));
    REQUIRE(EmatUsesLeafCards(ParseEmat("MatPBRBasic {\n BCRMap \"{1}a_BCR.edds\"\n OpacityMap \"{2}a_A.edds\"\n}\n")));
    REQUIRE(EmatUsesLeafCards(ParseEmat("MatPBRBasic {\n Cull none\n AlphaTest 1\n BCRMap \"{1}flag_BCR.edds\"\n}\n")));
    REQUIRE_FALSE(EmatUsesLeafCards(ParseEmat("MatPBRBasic {\n AlphaTest 0\n BCRMap \"{1}a_BCR.edds\"\n}\n")));
    REQUIRE_FALSE(EmatUsesLeafCards(
        ParseEmat("MatPBRTreeTrunk {\n BCRMap \"{1}bark_BCR.edds\"\n NTCMap \"{2}bark_NMO.edds\"\n}\n")));
    // The 1,726 `_MLOD` far-LOD materials: MatPBRBasic, BCRMap only. Opaque.
    REQUIRE_FALSE(EmatUsesLeafCards(
        ParseEmat("MatPBRBasic {\n MetalnessScale 0\n BCRMap \"{1}Mineflags_USSR_01_MLOD_BCR.edds\"\n}\n")));
    REQUIRE_FALSE(EmatUsesLeafCards(ParseEmat(kBasicEmat)));
}

TEST_CASE("EMAT deploy: the deployed spelling is the converter's spelling", "[asset][material][emat][arf]")
{
    // `VirtualNameFor` in XobCommand.cpp: prefix + "\\" + source path lower-cased with
    // forward slashes turned. The engine rebuilds it from the raw path; a mismatch here
    // is a file the converter writes and the engine never finds.
    REQUIRE(DeployedEmatPath("reforger\\everon\\textures",
                             "Assets/Vegetation/Tree/Alnus_Glutinosa/Data/t_alnus_glutinosa_2f_polyplane.emat") ==
            "reforger\\everon\\textures\\assets\\vegetation\\tree\\alnus_glutinosa\\data\\t_alnus_glutinosa_2f_"
            "polyplane.emat");
    REQUIRE(DeployedEmatPath("root", "/Assets/X.emat") == "root\\assets\\x.emat");

    // The env var grammar: `;`-separated, trailing slashes dropped, `none` disables.
    REQUIRE(ParseEmatDeployRoots("none").empty());
    REQUIRE(ParseEmatDeployRoots("NONE").empty());
    const auto two = ParseEmatDeployRoots("a\\b\\;c/d/");
    REQUIRE(two.size() == 2);
    REQUIRE(two[0] == "a\\b");
    REQUIRE(two[1] == "c/d");
    REQUIRE(ParseEmatDeployRoots("reforger\\everon\\textures") ==
            std::vector<std::string>{"reforger\\everon\\textures"});
}

TEST_CASE("EMAT deploy: a raw Enfusion path opens the deployed file, and only through a root",
          "[asset][material][emat][arf]")
{
    // The 1,132 models already installed name `Assets/...emat`. This is the branch that
    // lets them open the file the converter deploys, without a re-export.
    const auto root = std::filesystem::temp_directory_path() / "poseidon_emat_deploy_root";
    const auto file =
        root / "assets" / "vegetation" / "tree" / "alnus_glutinosa" / "data" / "t_alnus_glutinosa_2f_polyplane.emat";
    std::filesystem::create_directories(file.parent_path());
    {
        std::ofstream out(file, std::ios::trunc);
        REQUIRE(out);
        out << WriteEmatText(MakeDeployedEmat(ParseEmat(kAlderCrownEmat), kAlderColourPaa, true, "NTCMap", ""));
    }
    const std::string raw = "Assets/Vegetation/Tree/Alnus_Glutinosa/Data/t_alnus_glutinosa_2f_polyplane.emat";

    DeployRootsScope scope;

    // With the fallback disabled (POSEIDON_EMAT_ROOTS=none): the raw path opens
    // nothing, which is what every Everon material did before -- and it must THROW,
    // so the engine caches a miss rather than an empty material that claims a family.
    SetEmatDeployRoots({});
    REQUIRE_THROWS(LoadTranslatedMaterial(raw));

    // With the root: the same string opens the deployed file.
    SetEmatDeployRoots({root.string()});
    const TranslatedMaterial material = LoadTranslatedMaterial(raw);
    REQUIRE(material.shaderFamily == "MatPBRTreeCrown");
    const TranslatedSlot* base = material.Find(MaterialSlot::BaseColour);
    REQUIRE(base != nullptr);
    REQUIRE(base->texture.path.canonical().find("polyplane_alnus_glutinosa_bcr_ca.paa") != std::string::npos);

    // A path already under the root is opened as written and never re-rooted.
    REQUIRE(Poseidon::Asset::Material::IsUnderEmatDeployRoot(root.string() + "\\assets\\x.emat"));
    REQUIRE_FALSE(Poseidon::Asset::Material::IsUnderEmatDeployRoot(raw));

    // The deployed file EXISTS as text, and every RVMAT consumer (TexMaterial's
    // constructor, ShapeDraw's alpha route, the wgpu leaf-card test) reaches
    // ParseRvMaterialFile with the section's material path. It must refuse an `.emat`
    // BEFORE opening it: handed to ParamFile::Parse, `MatPBRTreeCrown {` is a word
    // followed by '{' where '=' was expected, and the config parser reports that
    // through ErrorMessage -- a Critical-error line per material, exit(1) under
    // --strict. REQUIRE_THROWS alone would pass either way (the parse failure also
    // throws); the message pins WHICH throw it was.
    REQUIRE(Poseidon::Asset::Material::IsEmatMaterialPath(file.string()));
    REQUIRE(Poseidon::Asset::Material::IsEmatMaterialPath("A.EMAT"));
    REQUIRE_FALSE(Poseidon::Asset::Material::IsEmatMaterialPath("a.rvmat"));
    REQUIRE_FALSE(Poseidon::Asset::Material::IsEmatMaterialPath(".emat"));
    REQUIRE_THROWS_WITH(Poseidon::Asset::Material::ParseRvMaterialFile(file.string()),
                        Catch::Matchers::StartsWith("not an RVMAT"));

    std::filesystem::remove_all(root);
}

TEST_CASE("EMAT deploy: the shipped alder crown, parent and all, through the real corpus",
          "[asset][material][emat][arf][corpus]")
{
    // Guarded on the local corpus (`%TEMP%\rf\emat`, the 10,312 `.emat` extracted from
    // the Reforger paks); skipped where it is absent. The parent chain is resolved
    // through the SAME root fallback the game uses -- `Assets/Vegetation/_Data/
    // crown_base.emat` is a raw path too -- so this is the whole read path on real data.
    const char* temp = std::getenv("TEMP");
    if (!temp || !*temp)
        return;
    const auto corpus = std::filesystem::path(temp) / "rf" / "emat";
    const auto alder =
        corpus / "Assets" / "Vegetation" / "Tree" / "Alnus_Glutinosa" / "Data" / "t_alnus_glutinosa_2f_polyplane.emat";
    if (!std::filesystem::exists(alder))
        return;

    DeployRootsScope scope;
    SetEmatDeployRoots({corpus.string()});

    EmatMaterial emat;
    REQUIRE(ReadEmatFile(alder.string(), emat));
    REQUIRE(emat.className == "MatPBRTreeCrown");
    // Its own keys.
    REQUIRE(emat.TextureOf("BCRMap") == "Assets/Vegetation/_Polyplanes/polyplane_alnus_glutinosa_BCR.edds");
    REQUIRE(emat.TextureOf("OpacityMap") == "Assets/Vegetation/_Polyplanes/polyplane_alnus_glutinosa_A.edds");
    REQUIRE(emat.TextureOf("NTCMap") == "Assets/Vegetation/_Polyplanes/polyplane_alnus_glutinosa_NTC.edds");
    // A key only the parent (crown_base.emat) defines: inheritance ran through the root.
    REQUIRE(emat.parentPath.empty());
    REQUIRE(emat.Has("WetnessScale"));
    REQUIRE(EmatUsesLeafCards(emat));

    // And the converter's deployment of THIS file, through the real writer and the
    // real reader: still a MatPBRTreeCrown, still a leaf card, naming exactly the two
    // files the converter would have written and keeping the parent's keys.
    const EmatMaterial back =
        ParseEmat(WriteEmatText(MakeDeployedEmat(emat, kAlderColourPaa, true, "NTCMap", kAlderNormalPaa)));
    REQUIRE(back.valid());
    REQUIRE(back.className == "MatPBRTreeCrown");
    REQUIRE(back.TextureOf("BCRMap") == kAlderColourPaa);
    REQUIRE(back.TextureOf("NTCMap") == kAlderNormalPaa);
    REQUIRE_FALSE(back.Has("OpacityMap"));
    REQUIRE_FALSE(back.Has("WorldColorizeMap"));
    REQUIRE(back.Has("WetnessScale"));
    REQUIRE(back.FloatOr("AlphaTest", 0.0f) == 1.0f);
    REQUIRE(EmatUsesLeafCards(back));
    REQUIRE(TranslateEmat(back, "deployed", "").Find(MaterialSlot::NormalMap) != nullptr);

    // The premise the handover carried -- "foliage opacity lives in BCR alpha" -- is
    // false for this tree and for 463 of the 468 MatPBRTreeCrown files: the opacity is
    // the OpacityMap, and the converter already folds it into the `_ca` face texture.
    // What turned the leaves into solid planes was the 21:23 re-export overwriting the
    // 140 cutout `.paa` files without the FLAG tagg (PAAEncoder.cpp, MAT-047).
}
