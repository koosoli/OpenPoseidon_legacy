// test_rv_material_source.cpp - AST-014: one source IR for Real Virtuality materials.
//
// Content mirrors a real Arma 2 Super material: seven stages, procedural stand-ins
// among the file textures, uvSource on every stage, and the source spelling
// "emmisive". Authored rather than copied - the corpora are licensed data.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/Material/RvMaterialSource.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <string>

using Poseidon::Asset::Material::ParseRvMaterial;
using Poseidon::Asset::Material::RvMaterialSource;
using Poseidon::Asset::Material::RvTextureRef;

static const char* kSuper = R"CFG(
ambient[]={1.0,1.0,1.0,1.0};
diffuse[]={1.0,1.0,1.0,1.0};
forcedDiffuse[]={0.0,0.0,0.0,0.0};
emmisive[]={0.0,0.0,0.0,1.0};
specular[]={0.5,0.5,0.5,1.0};
specularPower=100.0;
PixelShaderID="Super";
VertexShaderID="Super";
someUnhandledKey="keepme";
class Stage1
{
	texture="ca\test\data\worker_no.tga";
	uvSource="tex";
	class uvTransform
	{
		aside[]={2.0,0.0,0.0};
		up[]={0.0,3.0,0.0};
		dir[]={0.0,0.0,0.0};
		pos[]={0.5,0.25,0.0};
	};
};
class Stage2
{
	texture="#(argb,8,8,3)color(0.5,0.5,0.5,1,DT)";
	uvSource="none";
	unhandledStageKey="alsokeepme";
};
class Stage5
{
	texture="ca\test\data\worker_smdi.tga";
	uvSource="tex";
};
)CFG";

static RvMaterialSource parse(const char* text, const char* origin = "test.rvmat")
{
    std::string  buffer(text);
    QIStream     in(buffer.data(), static_cast<int>(buffer.size()));
    ParamFile    file;
    file.Parse(in);
    return ParseRvMaterial(file, origin);
}

TEST_CASE("RvMaterialSource: shader identity and render state", "[asset][material][ast-014]")
{
    auto material = parse(kSuper);
    REQUIRE(material.pixelShaderId == "Super");
    REQUIRE(material.vertexShaderId == "Super");
    REQUIRE(material.specularPower == 100.0f);
    REQUIRE(material.specular[0] == 0.5f);

    // Spelled "emmisive" in every file in the corpus. Correcting the spelling here
    // would silently default the value on every material rather than read it.
    REQUIRE(material.emissive[3] == 1.0f);
    REQUIRE_FALSE(material.embedded);
    REQUIRE(material.origin == "test.rvmat");
}

TEST_CASE("RvMaterialSource: stage indices are preserved, not renumbered", "[asset][material][ast-014]")
{
    auto material = parse(kSuper);
    REQUIRE(material.stages.size() == 3);

    // Stage5 must stay stage 5. Compacting to 0,1,2 would silently move a texture
    // to a different slot, and which slot a stage occupies is what a shader schema
    // keys on.
    REQUIRE(material.FindStage(1) != nullptr);
    REQUIRE(material.FindStage(2) != nullptr);
    REQUIRE(material.FindStage(5) != nullptr);
    REQUIRE(material.FindStage(3) == nullptr);
    REQUIRE(material.FindStage(5)->texture.path.canonical() == "ca\\test\\data\\worker_smdi.tga");
}

TEST_CASE("RvMaterialSource: procedural textures are not treated as paths", "[asset][material][ast-014]")
{
    auto material = parse(kSuper);
    const auto* generated = material.FindStage(2);
    REQUIRE(generated != nullptr);
    REQUIRE(generated->texture.isProcedural);
    REQUIRE(generated->texture.path.empty());
    REQUIRE(generated->texture.raw == "#(argb,8,8,3)color(0.5,0.5,0.5,1,DT)");

    // Only the real files count as dependencies. Reporting the procedural ones as
    // missing textures would flag roughly a third of a Super material's stages.
    REQUIRE(material.TextureDependencies().size() == 2);
}

TEST_CASE("RvMaterialSource: uvSource and uvTransform survive", "[asset][material][ast-014]")
{
    auto        material = parse(kSuper);
    const auto* stage    = material.FindStage(1);
    REQUIRE(stage != nullptr);
    REQUIRE(stage->uvSource == "tex");
    REQUIRE(stage->uvTransform.present);
    REQUIRE(stage->uvTransform.aside[0] == 2.0f);
    REQUIRE(stage->uvTransform.up[1] == 3.0f);
    REQUIRE(stage->uvTransform.pos[0] == 0.5f);

    // "none" is a real value, distinct from absent.
    REQUIRE(material.FindStage(2)->uvSource == "none");
    REQUIRE_FALSE(material.FindStage(2)->uvTransform.present);
}

TEST_CASE("RvMaterialSource: unrecognised keys are kept, not dropped", "[asset][material][ast-014]")
{
    auto material = parse(kSuper);

    bool sawTop = false;
    for (const auto& kv : material.extra)
        if (kv.first == "someUnhandledKey" && kv.second == "keepme")
            sawTop = true;
    REQUIRE(sawTop);

    bool sawStage = false;
    for (const auto& kv : material.FindStage(2)->extra)
        if (kv.first == "unhandledStageKey" && kv.second == "alsokeepme")
            sawStage = true;
    REQUIRE(sawStage);
}

TEST_CASE("RvMaterialSource: no meaning is assigned to a stage index", "[asset][material][ast-014]")
{
    // The IR exposes what the material said and nothing more. There is deliberately
    // no "normalMap()" accessor: which stage carries a normal map depends on the
    // shader family, and the indexed corpus holds 1,217 non-Super materials for
    // which any such shortcut would be wrong. That mapping is MAT-030's job.
    auto material = parse(kSuper);
    for (const auto& stage : material.stages)
        REQUIRE(stage.index >= 0);
    REQUIRE(material.pixelShaderId == "Super"); // the schema key, carried not interpreted
}

TEST_CASE("RvMaterialSource: TexGen blocks are not mistaken for stages", "[asset][material][ast-014]")
{
    // `TexGen0` carries a trailing index exactly as `Stage1` does, so anything
    // treating "indexed class" as "stage" swallows it. The indexed corpus holds 56
    // of them; they would surface as textureless stages, and the generator a shader
    // schema went looking for would simply be absent.
    static const char* kWithTexGen = R"CFG(
PixelShaderID="NormalMapSpecularDIMap";
class Stage1
{
	texture="ca\test\data\x_nohq.tga";
	uvSource="tex1";
};
class TexGen0
{
	uvSource="tex";
	class uvTransform
	{
		aside[]={1,0,0};
		up[]={0,1,0};
		dir[]={0,0,1};
		pos[]={0,0,0};
	};
};
class SomethingElse
{
	value=1;
};
)CFG";
    auto material = parse(kWithTexGen);

    REQUIRE(material.stages.size() == 1);
    REQUIRE(material.stages[0].index == 1);
    REQUIRE(material.texGens.size() == 1);
    REQUIRE(material.texGens[0].index == 0);
    REQUIRE(material.texGens[0].uvSource == "tex");
    REQUIRE(material.texGens[0].uvTransform.present);
    REQUIRE(material.texGens[0].uvTransform.dir[2] == 1.0f);

    // An unrelated class is neither, and is recorded rather than dropped.
    REQUIRE(material.otherClasses.size() == 1);
    REQUIRE(material.otherClasses[0] == "SomethingElse");

    // uvSource "tex1" names the second UV channel -- exactly what AST-011C
    // preserved. This is the join between the two tickets.
    REQUIRE(material.stages[0].uvSource == "tex1");
}
