// test_shader_schema.cpp - MAT-030: the first shader-family schema (Super).
//
// The stage->slot table was measured across the 2,853 Super materials in the
// indexed corpora, not assumed. Procedural stages name their own role (`...,DT)`)
// and file textures carry a suffix, so two independent witnesses agree on it.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/Material/MaterialChannels.hpp>
#include <Poseidon/Asset/Formats/Material/ShaderSchema.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <string>
#include <vector>

using namespace Poseidon::Asset::Material;

static MaterialIR translate(const char* text)
{
    std::string buffer(text);
    QIStream    in(buffer.data(), static_cast<int>(buffer.size()));
    ParamFile   file;
    file.Parse(in);
    return TranslateMaterial(ParseRvMaterial(file, "test.rvmat"));
}

static TranslatedMaterial translateSemantics(const char* text)
{
    std::string buffer(text);
    QIStream    in(buffer.data(), static_cast<int>(buffer.size()));
    ParamFile   file;
    file.Parse(in);
    const auto source = ParseRvMaterial(file, "test.rvmat");
    return TranslateSemantics(source, TranslateMaterial(source));
}

// Shaped like a real Arma 2 Super material, including the environment map at
// Stage7 whose filename claims to be a base colour.
static const char* kSuper = R"CFG(
PixelShaderID="Super";
class Stage1 { texture="ca\t\worker_nohq.tga"; uvSource="tex"; };
class Stage2 { texture="#(argb,8,8,3)color(0.5,0.5,0.5,1,DT)"; uvSource="tex1"; };
class Stage3 { texture="ca\t\worker_mc.tga"; uvSource="tex1"; };
class Stage4 { texture="#(argb,8,8,3)color(1,1,1,1,AS)"; uvSource="tex"; };
class Stage5 { texture="ca\\t\\worker_smdi.tga"; uvSource="tex"; };
class Stage6 { texture="#(ai,64,64,1)fresnel(2.0,0.1)"; uvSource="none"; };
class Stage7 { texture="ca\data\env_land_co.tga"; uvSource="none"; };
)CFG";

TEST_CASE("Super schema: stages bind to named slots", "[asset][material][mat-030]")
{
    auto ir = translate(kSuper);
    REQUIRE(ir.schemaKnown);
    REQUIRE(ir.shaderFamily == "Super");

    REQUIRE(ir.Slot(MaterialSlot::NormalMap).present);
    REQUIRE(ir.Slot(MaterialSlot::NormalMap).sourceStage == 1);
    REQUIRE(ir.Slot(MaterialSlot::Detail).sourceStage == 2);
    REQUIRE(ir.Slot(MaterialSlot::Macro).sourceStage == 3);
    REQUIRE(ir.Slot(MaterialSlot::AmbientShadow).sourceStage == 4);
    REQUIRE(ir.Slot(MaterialSlot::SpecularDetail).sourceStage == 5);
    REQUIRE(ir.Slot(MaterialSlot::Fresnel).sourceStage == 6);
    REQUIRE(ir.Slot(MaterialSlot::Environment).sourceStage == 7);

    // Absent, not defaulted to something: this material declares no Stage0.
    REQUIRE_FALSE(ir.Slot(MaterialSlot::BaseColour).present);
    REQUIRE(ir.unmappedStages.empty());
}

TEST_CASE("Super schema: the slot decides the role, not the filename", "[asset][material][mat-030]")
{
    auto ir = translate(kSuper);
    const auto& environment = ir.Slot(MaterialSlot::Environment);

    // env_land_co.tga is named `_co`. Suffix classification calls that a base
    // colour; it is the environment map. The slot wins and the disagreement is
    // reported rather than silently resolved either way.
    REQUIRE(environment.present);
    REQUIRE(environment.suffixRole == "BaseColour");
    REQUIRE_FALSE(environment.suffixAgrees);
    REQUIRE(ir.suffixDisagreements.size() == 1);
    REQUIRE(ir.suffixDisagreements[0] == MaterialSlot::Environment);

    // Where the filename does agree, it corroborates the schema.
    REQUIRE(ir.Slot(MaterialSlot::NormalMap).suffixAgrees);
    REQUIRE(ir.Slot(MaterialSlot::SpecularDetail).suffixRole == "SpecularDetail");
}

TEST_CASE("Super schema: SMDI stays SMDI", "[asset][material][mat-030]")
{
    // The roadmap is explicit that SMDI must not be mapped to roughness. This layer
    // assigns a slot and reinterprets no value; converting it is MAT-020's call and
    // needs its own evidence.
    auto ir = translate(kSuper);
    REQUIRE(ir.Slot(MaterialSlot::SpecularDetail).texture.path.canonical() ==
            "ca\\t\\worker_smdi.tga");
}

TEST_CASE("Super schema: uvSource survives to the slot", "[asset][material][mat-030]")
{
    // "tex1" selects the second UV channel AST-011C preserved; 211 Stage4 bindings
    // in the corpus use it. Losing it here would silently sample channel 0.
    auto ir = translate(kSuper);
    REQUIRE(ir.Slot(MaterialSlot::Detail).uvSource == "tex1");
    REQUIRE(ir.Slot(MaterialSlot::Macro).uvSource == "tex1");
    REQUIRE(ir.Slot(MaterialSlot::NormalMap).uvSource == "tex");
    REQUIRE(ir.Slot(MaterialSlot::Fresnel).uvSource == "none");
}

TEST_CASE("Super schema: procedural stages bind without a filename", "[asset][material][mat-030]")
{
    auto ir = translate(kSuper);
    const auto& detail = ir.Slot(MaterialSlot::Detail);
    REQUIRE(detail.present);
    REQUIRE(detail.texture.isProcedural);
    REQUIRE(detail.texture.path.empty());
    // No filename means no second opinion, which is silence and not dissent.
    REQUIRE(detail.suffixRole.empty());
    REQUIRE(detail.suffixAgrees);
}

TEST_CASE("Normal schema: Stage0 is the diffuse base colour", "[asset][material][mat-030]")
{
    // BI documents Normal as diffuse-colour modulation. It is not a one-stage
    // normal-map family, so Stage0 must not be guessed as NormalMap.
    auto ir = translate(R"CFG(
PixelShaderID="Normal";
class Stage0 { texture="ca\t\panel_co.paa"; uvSource="tex1"; };
)CFG");
    REQUIRE(ir.schemaKnown);
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).present);
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).sourceStage == 0);
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).uvSource == "tex1");
    REQUIRE_FALSE(ir.Slot(MaterialSlot::NormalMap).present);
    REQUIRE(ir.unmappedStages.empty());
}

TEST_CASE("NormalMap schema: only documented Stage1 becomes a normal map", "[asset][material][mat-030]")
{
    auto ir = translate(R"CFG(
PixelShaderID="NormalMap";
class Stage1 { texture="ca\t\panel_nohq.paa"; uvSource="tex"; };
class Stage3 { texture="#(ai,32,128,1)irradiance(8)"; uvSource="none"; };
)CFG");
    REQUIRE(ir.schemaKnown);
    REQUIRE(ir.Slot(MaterialSlot::NormalMap).present);
    REQUIRE(ir.Slot(MaterialSlot::NormalMap).sourceStage == 1);
    REQUIRE_FALSE(ir.Slot(MaterialSlot::BaseColour).present);
    // Irradiance has no semantic slot in this renderer yet: preserve its stage
    // as unmapped rather than misclassifying it as Fresnel or specular data.
    REQUIRE(ir.unmappedStages == std::vector<int>{3});
}

// The three fixtures below are the shipped Arma 3 materials, transcribed from the
// binarised originals in a3\plants_f\tree\data. The stage->slot table they exercise
// was measured across all 547 TreeAdv-family materials in the four shipped
// vegetation PBOs, not read off these three.

TEST_CASE("TreeAdvTrunk schema: bark normal and MCA bind, colour stays face-supplied",
          "[asset][material][mat-030][treeadv]")
{
    // t_pinusp3s_f_lod1_noatlas.rvmat: the trunk material of the Arma 3 pine.
    auto ir = translate(R"CFG(
PixelShaderID="TreeAdvTrunk";
VertexShaderID="TreeAdvTrunk";
class Stage1 { texture="a3\plants_f\_bark\bark_pinussylvestris_l3_nohq.paa"; uvSource="tex"; };
class Stage2 { texture="a3\plants_f\tree\data\t_pinusp3s_f_lod1_noatlas_mca.paa"; uvSource="tex1"; };
)CFG");
    REQUIRE(ir.schemaKnown);
    REQUIRE(ir.shaderFamily == "TreeAdvTrunk");

    const auto& normal = ir.Slot(MaterialSlot::NormalMap);
    REQUIRE(normal.present);
    REQUIRE(normal.sourceStage == 1);
    REQUIRE(normal.uvSource == "tex");
    REQUIRE(normal.suffixAgrees);

    // MCA is its own slot. Folding it into Super's Macro would assert the two are
    // the same texture semantic, which no evidence here establishes.
    const auto& mca = ir.Slot(MaterialSlot::MacroAmbient);
    REQUIRE(mca.present);
    REQUIRE(mca.sourceStage == 2);
    REQUIRE(mca.uvSource == "tex1"); // 169 of 172 trunk materials use the second UV set
    REQUIRE(mca.suffixRole == "MacroAmbient");

    // 168 of 172 trunk materials declare no Stage0; the face texture is the colour.
    REQUIRE_FALSE(ir.Slot(MaterialSlot::BaseColour).present);
    REQUIRE(ir.unmappedStages.empty());
    REQUIRE(ir.suffixDisagreements.empty());
}

TEST_CASE("TreeAdv schema: procedural foliage stages bind by tag, not filename", "[asset][material][mat-030][treeadv]")
{
    // t_pinusp3s_f_lod1_polyplane.rvmat: foliage. Both stages are procedural, and a
    // procedural stage names its own role -- the second independent witness that the
    // measured table is right.
    auto ir = translate(R"CFG(
PixelShaderID="TreeAdv";
VertexShaderID="TreeAdvModNormals";
class Stage1 { texture="#(argb,8,8,3)color(0.5,0.5,1.0,1,nohq)"; uvSource="tex"; };
class Stage2 { texture="#(argb,8,8,3)color(0.5,0.5,0.5,1.0,MCA)"; uvSource="tex1"; };
)CFG");
    REQUIRE(ir.schemaKnown);

    const auto& normal = ir.Slot(MaterialSlot::NormalMap);
    REQUIRE(normal.present);
    REQUIRE(normal.sourceStage == 1);
    REQUIRE(normal.texture.isProcedural);
    // A flat (0.5,0.5,1) normal is the source asking for unperturbed shading. That
    // is a value the renderer must honour, not a missing texture to substitute for.
    REQUIRE(normal.texture.path.empty());

    REQUIRE(ir.Slot(MaterialSlot::MacroAmbient).present);
    REQUIRE(ir.Slot(MaterialSlot::MacroAmbient).texture.isProcedural);
    REQUIRE_FALSE(ir.Slot(MaterialSlot::BaseColour).present);
    REQUIRE(ir.unmappedStages.empty());
}

TEST_CASE("TreeAdv schema: an _mc Stage2 keeps its slot and reports the disagreement",
          "[asset][material][mat-030][treeadv]")
{
    // 47 of the 547 corpus materials put an `_mc`-suffixed file in Stage2. The slot
    // the family assigns still decides the role; the suffix only gets to object.
    auto ir = translate(R"CFG(
PixelShaderID="TreeAdv";
class Stage1 { texture="a3\plants_f\_bark\bark_x_nohq.paa"; uvSource="tex"; };
class Stage2 { texture="a3\plants_f\tree\data\t_x_mc.paa"; uvSource="tex1"; };
)CFG");
    const auto& mca = ir.Slot(MaterialSlot::MacroAmbient);
    REQUIRE(mca.present);
    REQUIRE(mca.sourceStage == 2);
    REQUIRE(mca.suffixRole == "Macro");
    REQUIRE_FALSE(mca.suffixAgrees);
    REQUIRE(ir.suffixDisagreements == std::vector<MaterialSlot>{MaterialSlot::MacroAmbient});
}

TEST_CASE("TreeAdv schema: stages the family does not describe stay unmapped", "[asset][material][mat-030][treeadv]")
{
    // Exactly one TreeAdv material in the corpus declares Stage3..Stage7. One witness
    // does not describe a family, so those stages are preserved as unmapped instead
    // of borrowing Super's table for them.
    auto ir = translate(R"CFG(
PixelShaderID="TreeAdv";
class Stage1 { texture="a3\x_nohq.paa"; uvSource="tex"; };
class Stage2 { texture="#(argb,8,8,3)color(0.5,0.5,0.5,1,mca)"; uvSource="tex1"; };
class Stage3 { texture="#(argb,8,8,3)color(0.5,0.5,0.5,1,MC)"; uvSource="tex"; };
class Stage4 { texture="#(argb,8,8,3)color(1,1,1,1,AS)"; uvSource="tex"; };
class Stage7 { texture="a3\data_f\env_land_co.paa"; uvSource="tex"; };
)CFG");
    REQUIRE(ir.schemaKnown);
    REQUIRE(ir.Slot(MaterialSlot::NormalMap).present);
    REQUIRE(ir.Slot(MaterialSlot::MacroAmbient).present);
    REQUIRE_FALSE(ir.Slot(MaterialSlot::Macro).present);
    REQUIRE_FALSE(ir.Slot(MaterialSlot::AmbientShadow).present);
    REQUIRE_FALSE(ir.Slot(MaterialSlot::Environment).present);
    REQUIRE(ir.unmappedStages == std::vector<int>{3, 4, 7});
}

TEST_CASE("TreeAdvTrunk schema: Stage0 binds the colour the minority declares", "[asset][material][mat-030][treeadv]")
{
    // 4 of 172 trunk materials do declare a `_co` Stage0. Unlike TreeAdv, where
    // Stage0 is absent in every one of 369, the trunk family uses the stage.
    auto ir = translate(R"CFG(
PixelShaderID="TreeAdvTrunk";
class Stage0 { texture="a3\plants_f\_bark\bark_x_co.paa"; uvSource="tex1"; };
class Stage1 { texture="a3\plants_f\_bark\bark_x_nohq.paa"; uvSource="tex"; };
class Stage2 { texture="a3\plants_f\tree\data\t_x_mca.paa"; uvSource="tex1"; };
)CFG");
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).present);
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).sourceStage == 0);
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).uvSource == "tex1");
    REQUIRE(ir.suffixDisagreements.empty());
}

TEST_CASE("TreeAdv schema: MCA carries colour in RGB and occlusion in alpha", "[asset][material][mat-030][treeadv]")
{
    // Measured over the 181 `_mca` textures in the shipped vegetation PBOs: RGB
    // varies in 179 and is chromatic; alpha exists only in the 53 DXT5 files and
    // varies in 47 of them, while the 128 DXT1 files decode to a constant 255 --
    // an occlusion term of "none", which is why declaring the channel is safe for
    // them. Alpha here is occlusion, not transparency, and must not be read as one.
    const auto translated = translateSemantics(R"CFG(
PixelShaderID="TreeAdvTrunk";
class Stage1 { texture="a3\plants_f\_bark\bark_x_nohq.paa"; uvSource="tex"; };
class Stage2 { texture="a3\plants_f\tree\data\t_x_mca.paa"; uvSource="tex1"; };
)CFG");

    const auto* mca = translated.Find(MaterialSlot::MacroAmbient);
    REQUIRE(mca != nullptr);
    REQUIRE(mca->channels.colour == TextureChannel::R);
    REQUIRE(mca->channels.ambientShadow == TextureChannel::A);
    // Deliberately not `alpha`: a consumer that treated this as transparency would
    // punch holes in bark wherever the occlusion term is dark.
    REQUIRE(mca->channels.alpha == TextureChannel::None);

    // The normal map beside it is measured, and still says so.
    REQUIRE(translated.Find(MaterialSlot::NormalMap)->channels.normalX == TextureChannel::A);
}

TEST_CASE("Skin schema: the base colour sits where Super puts macro", "[asset][material][mat-030][skin]")
{
    // Shaped like the shipped Arma 3 head materials. Measured over all 347 Skin
    // materials: Stage2 is MC in 346, Stage3 is a `_co` albedo in 340. Reading this
    // family as Super would put the head's albedo in Macro and its macro in Detail --
    // plausible, silent, and wrong.
    auto ir = translate(R"CFG(
PixelShaderID="Skin";
class Stage1 { texture="a3\characters_f\heads\data\m_white_01_nohq.paa"; uvSource="tex"; };
class Stage2 { texture="a3\characters_f\heads\data\m_w1_mc.paa"; uvSource="tex"; };
class Stage3 { texture="a3\characters_f\heads\data\m_greek_01_co.paa"; uvSource="tex"; };
class Stage4 { texture="a3\characters_f\heads\data\m_white_01_as.paa"; uvSource="tex"; };
class Stage5 { texture="a3\characters_f\heads\data\m_white_01_smdi.paa"; uvSource="tex"; };
class Stage6 { texture="#(ai,64,64,1)fresnel(0.5,0.3)"; uvSource="none"; };
)CFG");
    REQUIRE(ir.schemaKnown);
    REQUIRE(ir.Slot(MaterialSlot::NormalMap).sourceStage == 1);
    REQUIRE(ir.Slot(MaterialSlot::Macro).sourceStage == 2);
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).sourceStage == 3);
    REQUIRE(ir.Slot(MaterialSlot::AmbientShadow).sourceStage == 4);
    REQUIRE(ir.Slot(MaterialSlot::SpecularDetail).sourceStage == 5);
    REQUIRE(ir.Slot(MaterialSlot::Fresnel).sourceStage == 6);

    // Super's Stage2 is Detail and its Stage3 is Macro. Neither is true here, and the
    // difference is the entire reason this family has its own table.
    REQUIRE_FALSE(ir.Slot(MaterialSlot::Detail).present);

    // Every suffix corroborates the slot, unlike Super's lying Stage7.
    REQUIRE(ir.suffixDisagreements.empty());
    REQUIRE(ir.unmappedStages.empty());
}

TEST_CASE("Skin schema: Stage7 stays unmapped even though it looks like Super's", "[asset][material][mat-030][skin]")
{
    // Both Stage7 occurrences in 347 materials are a3\data_f\env_co.paa -- the same
    // environment map Super carries there, `_co` name and all. Two materials still do
    // not describe a family, so the stage is preserved as unmapped rather than borrowed.
    auto ir = translate(R"CFG(
PixelShaderID="Skin";
class Stage1 { texture="a3\characters_f\heads\data\m_white_01_nohq.paa"; uvSource="tex"; };
class Stage7 { texture="a3\data_f\env_co.paa"; uvSource="none"; };
)CFG");
    REQUIRE(ir.schemaKnown);
    REQUIRE(ir.Slot(MaterialSlot::NormalMap).present);
    REQUIRE_FALSE(ir.Slot(MaterialSlot::Environment).present);
    REQUIRE(ir.unmappedStages == std::vector<int>{7});
}

TEST_CASE("An unknown shader family binds nothing", "[asset][material][mat-030]")
{
    // The roadmap forbids silently guessing unknown shader stages. A family with no
    // registered schema leaves every slot empty and reports its stages as unmapped;
    // the source IR still holds everything the material declared.
    static const char* kUnknown = R"CFG(
PixelShaderID="SomeFutureShader";
class Stage1 { texture="ca\t\x_nohq.tga"; uvSource="tex"; };
class Stage2 { texture="ca\t\x_dt.tga"; uvSource="tex"; };
)CFG";
    auto ir = translate(kUnknown);

    REQUIRE_FALSE(ir.schemaKnown);
    REQUIRE(ir.shaderFamily == "SomeFutureShader");
    REQUIRE_FALSE(ir.Slot(MaterialSlot::NormalMap).present);
    REQUIRE(ir.unmappedStages.size() == 2);
}

TEST_CASE("Super schema: a stage beyond the table is unmapped, not dropped", "[asset][material][mat-030]")
{
    static const char* kExtra = R"CFG(
PixelShaderID="Super";
class Stage1 { texture="ca\t\x_nohq.tga"; uvSource="tex"; };
class Stage9 { texture="ca\t\x_mask.tga"; uvSource="tex"; };
)CFG";
    auto ir = translate(kExtra);
    REQUIRE(ir.schemaKnown);
    REQUIRE(ir.Slot(MaterialSlot::NormalMap).present);
    REQUIRE(ir.unmappedStages.size() == 1);
    REQUIRE(ir.unmappedStages[0] == 9);
}

TEST_CASE("Multi schema: layer 0 stands in for the base colour, the mask is preserved", "[asset][material][mat-030][multi]")
{
    // Shaped like the shipped Arma 3 structure materials. Measured over all 1,628 Multi
    // materials: Stage0 present 1624/1628, Stage4 is a `_mask` in 1576. BI's Multimaterial
    // documentation describes four layers blended by a Stage4 mask.
    auto ir = translate(R"CFG(
PixelShaderID="Multi";
class Stage0 { texture="a3\structures_f\civ\constructions\data\wip_bricks_co.paa"; };
class Stage1 { texture="a3\structures_f\civ\constructions\data\wip_wood_co.paa"; };
class Stage2 { texture="a3\structures_f\civ\constructions\data\wip_cinderblocks_co.paa"; };
class Stage3 { texture="#(argb,8,8,3)color(0,0,0,1,CO)"; };
class Stage4 { texture="a3\structures_f\civ\constructions\data\wip_mask.paa"; };
class Stage5 { texture="a3\structures_f\civ\constructions\data\wip_cinderblocks_dtsmdi.paa"; };
)CFG");
    REQUIRE(ir.schemaKnown);

    // Layer 0 is bound so an Arma 3 structure renders as its first layer rather than as
    // untextured white. This is explicitly partial, not a claim about the blend.
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).present);
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).sourceStage == 0);

    // The blend control is named rather than dropped, and nothing samples it yet.
    REQUIRE(ir.Slot(MaterialSlot::Mask).present);
    REQUIRE(ir.Slot(MaterialSlot::Mask).sourceStage == 4);
    REQUIRE(ir.Slot(MaterialSlot::Mask).suffixRole == "Mask");
    REQUIRE(ir.Slot(MaterialSlot::Mask).suffixAgrees);

    // Layer 0's `_dtsmdi` is bound as the surface's specular, PAIRED with layer 0's colour
    // at Stage0. Its specular half uses SMDI's channels (measured over 72 textures), so it
    // needs no splitting guess -- the detail half is simply not sampled.
    REQUIRE(ir.Slot(MaterialSlot::SpecularDetail).present);
    REQUIRE(ir.Slot(MaterialSlot::SpecularDetail).sourceStage == 5);
    REQUIRE(ir.Slot(MaterialSlot::SpecularDetail).suffixRole == "SpecularDetail");
    REQUIRE(ir.Slot(MaterialSlot::SpecularDetail).suffixAgrees);

    // Layers 1-3 are bound now (MAT-039). Only their detail+specular stages, 6-8,
    // remain unmapped, and this material declares none of them.
    REQUIRE(ir.Slot(MaterialSlot::LayerColour1).sourceStage == 1);
    REQUIRE(ir.Slot(MaterialSlot::LayerColour2).sourceStage == 2);
    REQUIRE(ir.Slot(MaterialSlot::LayerColour3).sourceStage == 3);
    REQUIRE(ir.unmappedStages.empty());
    REQUIRE_FALSE(ir.Slot(MaterialSlot::Detail).present);
}

TEST_CASE("Multi's macro and ambient shadow are per-surface; its normals are per layer",
          "[asset][material][mat-030][multi][mat-039]")
{
    // Stage9 macro and Stage10 ambient shadow really are whole-surface -- both sit on the
    // mask's own TexGen4. The normals are not: stages 11-14 pair with the colour stages
    // 0-3 by TexGen, one per layer. Measured on two generations independently, which is
    // what makes it a shared Real Virtuality convention rather than a per-title one:
    //
    //   OA   ca\structures_e\housek\data\house_k_5_multi.rvmat
    //          Stage0 rock\wall_01_co       texGen0 <-> Stage11 rock\wall_01_nohq       texGen0
    //          Stage1 loam\wall_brick01_co  texGen1 <-> Stage12 loam\wall_brick01_nohq  texGen1
    //   A3   a3\structures_f\civ\offices\data\office_exterior_multi.rvmat
    //          Stage0 plaster\stucco_001_co texGen0 <-> Stage11 plaster\stucco_001_nohq texGen0
    //          Stage1 plaster03_4x4_office_co texGen1 <-> Stage12 ..._nohq                texGen1
    //
    // Calling Stage11 "the surface normal" bound layer 0's map to the whole surface.
    auto ir = translate(R"CFG(
PixelShaderID="Multi";
class TexGen0
{
	uvSource="tex";
	class uvTransform { aside[]={4,0,0}; up[]={0,4,0}; dir[]={0,0,0}; pos[]={0,0,0}; };
};
class TexGen1
{
	uvSource="tex";
	class uvTransform { aside[]={16,0,0}; up[]={0,16,0}; dir[]={0,0,0}; pos[]={0,0,0}; };
};
class Stage0 { texture="a3\structures_f\data\wall_04_co.paa"; texGen=0; };
class Stage1 { texture="a3\structures_f\datarick_04_co.paa"; texGen=1; };
class Stage4 { texture="a3\structures_f\data\wall_04_mask.paa"; };
class Stage5 { texture="a3\structures_f\data\wall_04_dtsmdi.paa"; };
class Stage9 { texture="a3\structures_f\data\wall_04_mc.paa"; };
class Stage10 { texture="a3\structures_f\data\wall_04_ads.paa"; };
class Stage11 { texture="a3\structures_f\data\wall_04_nohq.paa"; texGen=0; };
class Stage12 { texture="a3\structures_f\datarick_04_nohq.paa"; texGen=1; };
)CFG");
    REQUIRE(ir.schemaKnown);
    REQUIRE(ir.Slot(MaterialSlot::Macro).sourceStage == 9);
    REQUIRE(ir.Slot(MaterialSlot::AmbientShadow).sourceStage == 10);
    REQUIRE(ir.Slot(MaterialSlot::NormalMap).sourceStage == 11);
    REQUIRE(ir.Slot(MaterialSlot::LayerNormal1).sourceStage == 12);

    // The per-layer TexGen is the point: layer 1 tiles four times as densely as layer 0,
    // and a translation that kept only `uvSource` collapsed both onto one frame.
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).uvTransform.present);
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).uvTransform.aside[0] == 4.0f);
    REQUIRE(ir.Slot(MaterialSlot::LayerColour1).uvTransform.aside[0] == 16.0f);
    REQUIRE(ir.Slot(MaterialSlot::LayerNormal1).uvTransform.aside[0] == 16.0f);

    // `_ads` is the majority spelling of the ambient-shadow suffix in this family (808 of
    // 1,628), and went unrecognised until now -- so the second witness stayed silent on the
    // commonest form of the commonest layered material.
    REQUIRE(ir.Slot(MaterialSlot::AmbientShadow).suffixRole == "AmbientShadow");
    REQUIRE(ir.suffixDisagreements.empty());
}

TEST_CASE("SuperAToC schema: Super's table, and the cutout colour it carries at Stage0",
          "[asset][material][mat-030][superatoc]")
{
    // Shaped like a3\structures_f\walls\data\wired_fence_wire.rvmat. Measured over all 8:
    // Stage0 colour 7/7 (6 `_ca`), then Super's table stage for stage. Stage0 is why it
    // matters -- these are wire fences whose colour and cutout alpha are in the material.
    auto ir = translate(R"CFG(
PixelShaderID="SuperAToC";
class Stage0 { texture="a3\structures_f\walls\data\metal_fence_wire_ca.paa"; uvSource="tex"; };
class Stage1 { texture="#(argb,8,8,3)color(0.5,0.5,1,1,NOHQ)"; uvSource="tex"; };
class Stage2 { texture="#(argb,8,8,3)color(1,1,1,1,DTSMDI)"; uvSource="tex"; };
class Stage3 { texture="#(argb,8,8,3)color(0,0,0,0,MC)"; uvSource="tex1"; };
class Stage4 { texture="#(argb,8,8,3)color(1,1,1,1,AS)"; uvSource="tex1"; };
class Stage5 { texture="#(argb,8,8,3)color(0,0,1,1,SMDI)"; uvSource="tex"; };
class Stage6 { texture="#(ai,64,64,1)fresnelGlass()"; uvSource="tex"; };
class Stage7 { texture="a3\data_f\env_land_co.paa"; uvSource="tex"; };
)CFG");
    REQUIRE(ir.schemaKnown);
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).present);
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).sourceStage == 0);
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).suffixRole == "BaseColour");
    REQUIRE(ir.Slot(MaterialSlot::NormalMap).sourceStage == 1);
    REQUIRE(ir.Slot(MaterialSlot::SpecularDetail).sourceStage == 5);
    REQUIRE(ir.Slot(MaterialSlot::Environment).sourceStage == 7);
    REQUIRE(ir.unmappedStages.empty());
}

TEST_CASE("SuperAToC is matched however the corpus spells it", "[asset][material][mat-030][superatoc]")
{
    // 7 materials write `SuperAToC` and 1 writes `SuperAtoc`. Family lookup is
    // case-insensitive, so they are one family rather than one schema and one hole.
    REQUIRE(translate(R"CFG(
PixelShaderID="SuperAtoc";
class Stage1 { texture="a3\structures_f\walls\data\new_wirefence_wire_nohq.paa"; uvSource="tex"; };
)CFG")
                .schemaKnown);
}

TEST_CASE("NormalMapSpecularDIMap schema: normal and SMDI, with an optional Stage0 colour",
          "[asset][material][mat-030][normalmap-cluster]")
{
    // a3\structures_f\walls\data\crash_barrier.rvmat, one of 111. Stage1 normal 111/111,
    // Stage2 SMDI 108/111, Stage0 `_co` in 13.
    auto ir = translate(R"CFG(
PixelShaderID="NormalMapSpecularDIMap";
class Stage0 { texture="a3\structures_f_enoch\industrial\mines\data\ind_coltan_hopper_co.paa"; uvSource="tex"; };
class Stage1 { texture="a3\structures_f\walls\data\crash_barrier_nohq.paa"; uvSource="tex"; };
class Stage2 { texture="a3\structures_f\walls\data\crash_barrier_smdi.paa"; uvSource="tex"; };
)CFG");
    REQUIRE(ir.schemaKnown);
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).sourceStage == 0);
    REQUIRE(ir.Slot(MaterialSlot::NormalMap).sourceStage == 1);
    REQUIRE(ir.Slot(MaterialSlot::SpecularDetail).sourceStage == 2);
    REQUIRE(ir.suffixDisagreements.empty());
}

TEST_CASE("The NormalMap* cluster reads its stage list off its own name",
          "[asset][material][mat-030][normalmap-cluster]")
{
    // Each family names the stages that follow Stage1, in order, and the corpus agrees for
    // every one of them. This is the property that makes the cluster a family rather than
    // four coincidences, so it is asserted as one.
    auto detailSpec = translate(R"CFG(
PixelShaderID="NormalMapDetailSpecularMap";
class Stage1 { texture="a3\roads_f\decals\data\decal_path_nohq.paa"; uvSource="tex"; };
class Stage2 { texture="#(argb,8,8,3)color(0.5,0.5,0.5,0.5,DT)"; uvSource="tex"; };
class Stage3 { texture="a3\roads_f\roads\data\path_sm.paa"; uvSource="tex"; };
)CFG");
    REQUIRE(detailSpec.schemaKnown);
    REQUIRE(detailSpec.Slot(MaterialSlot::NormalMap).sourceStage == 1);
    REQUIRE(detailSpec.Slot(MaterialSlot::Detail).sourceStage == 2);
    REQUIRE(detailSpec.Slot(MaterialSlot::SpecularDetail).sourceStage == 3);

    auto macroAs = translate(R"CFG(
PixelShaderID="NormalMapMacroASSpecularDIMap";
class Stage1 { texture="a3\structures_f_enoch\cultural\orthodoxchurches\data\church_03_strecha_nohq.paa"; uvSource="tex"; };
class Stage2 { texture="a3\structures_f_enoch\cultural\orthodoxchurches\data\church_03_mc.paa"; uvSource="tex1"; };
class Stage3 { texture="a3\structures_f_enoch\cultural\orthodoxchurches\data\church_02a_as.paa"; uvSource="tex1"; };
class Stage4 { texture="#(argb,8,8,3)color(0,0,1,1,SMDI)"; uvSource="tex"; };
)CFG");
    REQUIRE(macroAs.schemaKnown);
    REQUIRE(macroAs.Slot(MaterialSlot::Macro).sourceStage == 2);
    REQUIRE(macroAs.Slot(MaterialSlot::AmbientShadow).sourceStage == 3);
    REQUIRE(macroAs.Slot(MaterialSlot::SpecularDetail).sourceStage == 4);
}

TEST_CASE("NormalMapDiffuse puts its albedo at Stage2, where its siblings put specular",
          "[asset][material][mat-030][normalmap-cluster]")
{
    // The trap of the cluster, and the reason each family gets its own table. Measured over
    // all 27: Stage1 `_nopx` 26/27, Stage2 `_co` 26/27. Reading this as
    // NormalMapSpecularDIMap would bind a ground albedo into SpecularDetail.
    auto ir = translate(R"CFG(
PixelShaderID="NormalMapDiffuse";
class Stage1 { texture="a3\map_data\gdt_concrete_nopx.paa"; uvSource="tex"; };
class Stage2 { texture="a3\map_data\gdt_concrete_co.paa"; uvSource="tex"; };
)CFG");
    REQUIRE(ir.schemaKnown);
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).sourceStage == 2);
    REQUIRE_FALSE(ir.Slot(MaterialSlot::SpecularDetail).present);

    // `_nopx` is DXT5nm like `_nohq` (measured over 31 textures), so it agrees with the
    // NormalMap slot rather than reporting a suffix nobody recognises.
    REQUIRE(ir.Slot(MaterialSlot::NormalMap).sourceStage == 1);
    REQUIRE(ir.Slot(MaterialSlot::NormalMap).suffixRole == "NormalMap");
    REQUIRE(ir.suffixDisagreements.empty());
}

// Was "stays unmapped at n=2". DZ-001 raised the evidence: DayZ has 8 of these and
// all 8 agree on one shape -- Stage1 _nohq, Stage2 _dt, Stage3 _mc, Stage4 _as,
// Stage5 _smdi, i.e. Super tail without Super Stage0 colour. The old bar was "two
// materials do not describe a family", right on the evidence then; it is mapped now
// because the evidence changed, not because the name was tempting.
TEST_CASE("NormalMapDetailMacroASSpecularDIMap is mapped on DayZ evidence",
          "[asset][material][mat-030][dz-001][normalmap-cluster]")
{
    const MaterialIR ir = translate(R"CFG(
PixelShaderID="NormalMapDetailMacroASSpecularDIMap";
class Stage1 { texture="dz\data\case_d_nohq.paa"; uvSource="tex"; };
class Stage2 { texture="dz\data\case_d_dt.paa"; uvSource="tex"; };
class Stage5 { texture="dz\data\case_d_smdi.paa"; uvSource="tex"; };
)CFG");
    REQUIRE(ir.schemaKnown);
    REQUIRE(ir.Slot(MaterialSlot::NormalMap).sourceStage == 1);
    REQUIRE(ir.Slot(MaterialSlot::Detail).sourceStage == 2);
    REQUIRE(ir.Slot(MaterialSlot::SpecularDetail).sourceStage == 5);
    // Stage0 is absent in all 8, so colour comes from the face texture.
    REQUIRE_FALSE(ir.Slot(MaterialSlot::BaseColour).present);
}

// CalmWater rvmats are two lines -- the two shader ids, no stages, no colours. An
// empty stage list is the accurate description and stops the translator hunting for
// an albedo nobody authored. This does not make a pond look like water.
TEST_CASE("CalmWater is known and declares no stages", "[asset][material][dz-001]")
{
    const MaterialIR ir = translate(R"CFG(
PixelShaderID="CalmWater";
VertexShaderID="CalmWater";
)CFG");
    REQUIRE(ir.schemaKnown);
    REQUIRE_FALSE(ir.Slot(MaterialSlot::BaseColour).present);
    REQUIRE_FALSE(ir.Slot(MaterialSlot::NormalMap).present);
}

// DayZ Grass puts its colour at Stage3, not Stage0 -- the one family in the tail
// whose layout the name does not predict.
TEST_CASE("Grass carries colour at Stage3", "[asset][material][dz-001]")
{
    const MaterialIR ir = translate(R"CFG(
PixelShaderID="Grass";
class Stage2 { texture="dz\data\grass_nohq.paa"; uvSource="tex"; };
class Stage3 { texture="dz\data\grass_co.paa"; uvSource="tex"; };
)CFG");
    REQUIRE(ir.schemaKnown);
    REQUIRE(ir.Slot(MaterialSlot::NormalMap).sourceStage == 2);
    REQUIRE(ir.Slot(MaterialSlot::BaseColour).sourceStage == 3);
}

// Shaped like ca\structures_e\wall\wall_l\data\walls_l3.rvmat: the four layers ride TexGen0-3
// on `tex`, and the mask / macro / ambient-shadow stages ride TexGen4 on `tex1`. Every stage
// names its TexGen and carries no uvSource of its own, so the channel has to come THROUGH the
// TexGen link -- which is where it was being lost.
static const char* kMultiWall = R"CFG(
PixelShaderID="Multi";
VertexShaderID="Multi";
class TexGen0 { uvSource="tex"; class uvTransform { aside[]={1,0,0}; up[]={0,1,0}; dir[]={0,0,0}; pos[]={0,0,0}; }; };
class TexGen1 { uvSource="tex"; class uvTransform { aside[]={1,0,0}; up[]={0,1,0}; dir[]={0,0,0}; pos[]={0,0,0}; }; };
class TexGen2 { uvSource="tex"; class uvTransform { aside[]={1,0,0}; up[]={0,1,0}; dir[]={0,0,0}; pos[]={0,0,0}; }; };
class TexGen3 { uvSource="tex"; class uvTransform { aside[]={1,0,0}; up[]={0,1,0}; dir[]={0,0,0}; pos[]={0,0,0}; }; };
class TexGen4 { uvSource="tex1"; class uvTransform { aside[]={1,0,0}; up[]={0,1,0}; dir[]={0,0,0}; pos[]={0,0,0}; }; };
class Stage0 { texture="ca\structures_e\data\plaster\wall_plaster_02_co.paa"; texGen=0; };
class Stage1 { texture="ca\structures_e\data\loam\wall_dirt03_co.paa"; texGen=1; };
class Stage2 { texture="ca\structures_e\data\metal\metal03_co.paa"; texGen=2; };
class Stage3 { texture="ca\structures\data\metal\metalrust1_co.paa"; texGen=3; };
class Stage4 { texture="ca\structures_e\wall\wall_l\data\walls_l3_mask.paa"; texGen=4; };
class Stage9 { texture="ca\structures_e\wall\wall_l\data\walls_l3_mc.paa"; texGen=4; };
class Stage10 { texture="ca\structures_e\wall\wall_l\data\walls_l3_ads.paa"; texGen=4; };
)CFG";

TEST_CASE("Multi schema: the mask's uvSource comes through its TexGen", "[asset][material][mat-039]")
{
    // The owner's "gray stuff on the wall": the mask sampled on the tiling uv paints its
    // regions as rectangles across the wall. It has to report tex1 here or the renderer
    // cannot know to sample it with the second UV set.
    auto ir = translate(kMultiWall);
    REQUIRE(ir.Slot(MaterialSlot::Mask).present);
    CHECK(ir.Slot(MaterialSlot::Mask).uvSource == "tex1");
    CHECK(ir.Slot(MaterialSlot::Macro).uvSource == "tex1");
    CHECK(ir.Slot(MaterialSlot::AmbientShadow).uvSource == "tex1");
    CHECK(ir.Slot(MaterialSlot::BaseColour).uvSource == "tex");
    CHECK(ir.Slot(MaterialSlot::LayerColour1).uvSource == "tex");
}
