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

TEST_CASE("TreeAdv schema: MCA channel semantics are not claimed", "[asset][material][mat-030][treeadv]")
{
    // BI's tree documentation describes MCA as a macro in RGB with ambient occlusion
    // in alpha. That is documentation, not a measurement, and the AS/SMDI/NOHQ maps
    // in this codebase were measured. Until MCA is too, it claims no channel.
    const auto translated = translateSemantics(R"CFG(
PixelShaderID="TreeAdvTrunk";
class Stage1 { texture="a3\plants_f\_bark\bark_x_nohq.paa"; uvSource="tex"; };
class Stage2 { texture="a3\plants_f\tree\data\t_x_mca.paa"; uvSource="tex1"; };
)CFG");

    const auto* mca = translated.Find(MaterialSlot::MacroAmbient);
    REQUIRE(mca != nullptr);
    REQUIRE(mca->channels.colour == TextureChannel::None);
    REQUIRE(mca->channels.ambientShadow == TextureChannel::None);
    REQUIRE(mca->channels.alpha == TextureChannel::None);

    // The normal map beside it is measured, and still says so.
    REQUIRE(translated.Find(MaterialSlot::NormalMap)->channels.normalX == TextureChannel::A);
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
