// test_rv_material_source.cpp - AST-014: one source IR for Real Virtuality materials.
//
// Content mirrors a real Arma 2 Super material: seven stages, procedural stand-ins
// among the file textures, uvSource on every stage, and the source spelling
// "emmisive". Authored rather than copied - the corpora are licensed data.

#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Asset/Formats/Config/ArmaRap.hpp>
#include <Poseidon/Asset/Formats/Material/RvMaterialSource.hpp>
#include <Poseidon/World/Terrain/ObjectStreamRapStageNames.hpp>
#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/Streams/QStream.hpp>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <memory>
#include <string>

using Poseidon::Asset::Material::ParseRvMaterial;
using Poseidon::Asset::Material::RvMaterialSource;
using Poseidon::Asset::Material::RvTextureRef;

namespace
{
void appendString(std::vector<uint8_t>& out, const char* text)
{
    while (*text)
        out.push_back(static_cast<uint8_t>(*text++));
    out.push_back(0);
}

void appendU32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}

void writeU32(std::vector<uint8_t>& out, size_t offset, uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out[offset + shift / 8] = static_cast<uint8_t>(value >> shift);
}

std::vector<uint8_t> minimalRap(const char* stageTexture = "a3\\rocks_f\\data\\stone_nohq.paa",
    const char* shader = "Super", int stageIndex = 1)
{
    std::vector<uint8_t> out{0, 'r', 'a', 'P', 0, 0, 0, 0, 8, 0, 0, 0, 0x0a, 0x06, 0, 0};
    appendString(out, ""); // Root's binary parent-reference field.
    out.push_back(4);

    out.push_back(1);
    out.push_back(0);
    appendString(out, "PixelShaderID");
    appendString(out, shader);
    out.push_back(2);
    appendString(out, "numbers");
    out.push_back(2);
    out.push_back(2);
    appendU32(out, 42);
    out.push_back(0);
    appendString(out, "variable");
    out.push_back(0);
    appendString(out, "TexGen3");
    const size_t texGenOffset = out.size();
    appendU32(out, 0);

    out.push_back(0);
    const std::string stageName = "Stage" + std::to_string(stageIndex);
    appendString(out, stageName.c_str());
    const size_t stageOffset = out.size();
    appendU32(out, 0);

    const uint32_t texGenBodyOffset = static_cast<uint32_t>(out.size());
    writeU32(out, texGenOffset, texGenBodyOffset);
    appendString(out, "");
    out.push_back(2);
    out.push_back(1);
    out.push_back(0);
    appendString(out, "uvSource");
    appendString(out, "worldPos");
    out.push_back(0);
    appendString(out, "uvTransform");
    const size_t transformOffset = out.size();
    appendU32(out, 0);

    const uint32_t stageBodyOffset = static_cast<uint32_t>(out.size());
    writeU32(out, stageOffset, stageBodyOffset);
    appendString(out, "");
    out.push_back(3);
    out.push_back(1);
    out.push_back(0);
    appendString(out, "texture");
    appendString(out, stageTexture);
    out.push_back(1);
    out.push_back(0);
    appendString(out, "uvSource");
    appendString(out, "tex");
    out.push_back(1);
    out.push_back(2);
    appendString(out, "texGen");
    appendU32(out, 3);

    const uint32_t transformBodyOffset = static_cast<uint32_t>(out.size());
    writeU32(out, transformOffset, transformBodyOffset);
    appendString(out, "");
    out.push_back(2);
    out.push_back(2);
    appendString(out, "aside");
    out.push_back(3);
    for (const float value : {2.0f, 0.0f, 0.0f})
    {
        out.push_back(1);
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        appendU32(out, bits);
    }
    out.push_back(2);
    appendString(out, "up");
    out.push_back(3);
    for (const float value : {0.0f, 3.0f, 0.0f})
    {
        out.push_back(1);
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        appendU32(out, bits);
    }
    return out;
}
} // namespace

TEST_CASE("ArmaRapReader retains literal, array, and class members", "[asset][config][rap]")
{
    const auto root = Poseidon::Asset::Config::ArmaRapReader(minimalRap()).root();
    REQUIRE(root.size() == 4);
    REQUIRE(root[0].name == "PixelShaderID");
    REQUIRE(root[0].value.text == "Super");
    REQUIRE(root[1].name == "numbers");
    REQUIRE(root[1].value.array.size() == 2);
    REQUIRE(root[1].value.array[0].number == 42.0f);
    REQUIRE(root[1].value.array[1].text == "variable");
    REQUIRE(root[2].name == "TexGen3");
    REQUIRE(root[2].body.size() == 2);
    REQUIRE(root[3].name == "Stage1");
    REQUIRE(root[3].body.size() == 3);
    REQUIRE(root[3].body[0].name == "texture");
    REQUIRE(root[3].body[1].value.text == "tex");
}

TEST_CASE("RVMAT conversion is a narrow view over generic ArmaRapReader", "[asset][material][rap]")
{
    const auto material = Poseidon::Asset::Material::ParseArmaRapMaterial(minimalRap(), "synthetic.rvmat");
    REQUIRE(material.pixelShaderId == "Super");
    REQUIRE(material.stages.size() == 1);
    REQUIRE(material.stages[0].index == 1);
    REQUIRE(material.stages[0].texture.path.canonical() == "a3\\rocks_f\\data\\stone_nohq.paa");
    REQUIRE(material.stages[0].uvSource == "tex");
    REQUIRE(material.stages[0].texGen == 3);
    REQUIRE(material.texGens.size() == 1);
    REQUIRE(material.texGens[0].index == 3);
    REQUIRE(material.texGens[0].uvSource == "worldPos");
    REQUIRE(material.texGens[0].uvTransform.present);
    REQUIRE(material.texGens[0].uvTransform.aside[0] == 2.0f);
    REQUIRE(material.texGens[0].uvTransform.up[1] == 3.0f);
}

TEST_CASE("Owned raP stage names are complete or refused", "[preparer][rap-stage-names]")
{
    using namespace Poseidon::Streaming;
    auto extract = [](const std::vector<uint8_t>& input, std::string_view path = "A3/Rocks_F/Data/Stone.rvmat") {
        const std::vector<char> bytes(input.begin(), input.end());
        return ExtractOwnedRapStageNames(std::span<const char>(bytes.data(), bytes.size()), path);
    };

    const auto valid = extract(minimalRap());
    REQUIRE(valid.status == RapStageNamesStatus::Complete);
    REQUIRE(valid.names == std::vector<std::string>{"a3\\rocks_f\\data\\stone_nohq.paa"});
    CHECK(valid.normalMapName == "a3\\rocks_f\\data\\stone_nohq.paa");
    REQUIRE(valid.normalStages.size() == 1);
    CHECK(valid.normalStages[0].layer == 0);

    // Multi binds four typed normal layers in Stage11..14. A single authored
    // material may therefore contribute several measured first-touch PAAs.
    std::vector<uint8_t> layered{0,'r','a','P',0,0,0,0,8,0,0,0,0x0a,0x06,0,0};
    appendString(layered, ""); layered.push_back(5);
    layered.push_back(1); layered.push_back(0);
    appendString(layered, "PixelShaderID"); appendString(layered, "Multi");
    std::array<size_t,4> offsets{};
    for (size_t i=0;i<4;++i)
    {
        layered.push_back(0);
        const auto stage = "Stage" + std::to_string(11+i);
        appendString(layered, stage.c_str());
        offsets[i] = layered.size(); appendU32(layered,0);
    }
    for (size_t i=0;i<4;++i)
    {
        writeU32(layered,offsets[i],static_cast<uint32_t>(layered.size()));
        appendString(layered,""); layered.push_back(1);
        layered.push_back(1); layered.push_back(0); appendString(layered,"texture");
        const auto key = "dz\\structures\\data\\plaster\\plaster_flats0" +
            std::to_string(i+1) + "_nohq.paa";
        appendString(layered,key.c_str());
    }
    const auto multi = extract(layered,"dz\\structures\\residential\\tenements\\data\\tb_small_flats_walls.rvmat");
    REQUIRE(multi.status == RapStageNamesStatus::Complete);
    REQUIRE(multi.normalStages.size() == 4);
    CHECK(multi.normalMapName == multi.normalStages[0].name);
    for (uint8_t i=0;i<4;++i)
        CHECK(multi.normalStages[i].layer == i);

    const auto macro = extract(minimalRap("dz\\plants\\tree\\data\\trunk_dxt5.paa", "TreeAdvTrunk", 2));
    REQUIRE(macro.status == RapStageNamesStatus::Complete);
    CHECK(macro.names.size() == 1);
    CHECK(macro.normalMapName.empty()); // Stage2 has no WGPU MacroAmbient texture consumer.
    const auto unknownShader = extract(minimalRap("a3\\rocks_f\\data\\stone_nohq.paa", "UnknownShader"));
    CHECK(unknownShader.status == RapStageNamesStatus::Complete);
    CHECK(unknownShader.normalMapName.empty());

    const auto procedural = extract(minimalRap("#(argb,8,8,3)color(1,1,1,1)"));
    CHECK(procedural.status == RapStageNamesStatus::Complete);
    CHECK(procedural.names.empty());
    CHECK(procedural.normalMapName.empty());

    const auto unsupported = extract(minimalRap("a3/rocks_f/data/stone.png"));
    CHECK(unsupported.status == RapStageNamesStatus::Unsupported);
    CHECK(unsupported.names.empty());

    const auto text = extract(std::vector<uint8_t>{'c','l','a','s','s',' ','S','t','a','g','e','1'});
    CHECK(text.status == RapStageNamesStatus::NotRap);
    CHECK(text.names.empty());

    auto oversized = minimalRap();
    oversized.resize(256 * 1024 + 1);
    CHECK(extract(oversized).status == RapStageNamesStatus::OverLimit);
    CHECK(extract(oversized).names.empty());

    auto truncated = minimalRap();
    truncated.resize(16);
    CHECK(extract(truncated).status == RapStageNamesStatus::Invalid);
    CHECK(extract(truncated).names.empty());

    // A valid-looking class offset can point back to its own body. The bounded
    // raP reader must reject the cycle rather than recurse until stack overflow.
    std::vector<uint8_t> cycle{0, 'r', 'a', 'P', 0, 0, 0, 0, 8, 0, 0, 0, 10, 6, 0, 0};
    appendString(cycle, "");
    cycle.push_back(1); // one class member
    cycle.push_back(0); // class
    appendString(cycle, "Stage1");
    appendU32(cycle, 16); // points back to the root body
    CHECK(extract(cycle).status == RapStageNamesStatus::Invalid);
    CHECK(extract(cycle).names.empty());

    CHECK(extract(minimalRap(), "C:/host/stone.rvmat").status == RapStageNamesStatus::Unsupported);
}

TEST_CASE("raP owner candidate is bounded NormalMap metadata with physical material identity", "[preparer][rap-stage-names]")
{
    Poseidon::BankReadMemberIdentity member;
    member.archiveBytes = 4096; member.offset = 100; member.bytes = 925; member.fileId[0] = 17;
    Poseidon::RapStageCandidateHandoff handoff;
    CHECK_FALSE(handoff.Bounded());
    handoff.generation = 7; handoff.modelIndex = 3;
    handoff.materials.push_back({"dz\\plants\\tree\\data\\t_piceaabies_2d_trunk.rvmat",
        "dz\\plants\\tree\\data\\t_piceaabies_trunk_no.paa",
        Poseidon::RapStageCandidateHandoff::Consumer::NormalMap, member});
    REQUIRE(handoff.Bounded());
    auto moved = std::make_unique<Poseidon::RapStageCandidateHandoff>(std::move(handoff));
    REQUIRE(moved->Bounded());
    CHECK(moved->modelIndex == 3);
    CHECK(moved->materials[0].stageName == "dz\\plants\\tree\\data\\t_piceaabies_trunk_no.paa");
    CHECK(moved->materials[0].SameCurrentMember(member));
    auto replaced = member; ++replaced.offset;
    CHECK_FALSE(moved->materials[0].SameCurrentMember(replaced));
    moved->materials.push_back({"dz\\plants\\tree\\data\\d_piceaabies_stumpb_trunk_a.rvmat",
        "dz\\plants\\tree\\data\\t_piceaabies_trunk_no.paa",
        Poseidon::RapStageCandidateHandoff::Consumer::NormalMap, member});
    CHECK(moved->Bounded());
    moved->materials.push_back({"dz\\plants\\tree\\data\\d_piceaabies_stumpb_trunk_a.rvmat",
        "dz\\plants\\tree\\data\\t_piceaabies_trunk_dxt5.paa",
        Poseidon::RapStageCandidateHandoff::Consumer::NormalMap, member});
    CHECK_FALSE(moved->Bounded()); // duplicate typed slot within the same material
    moved->materials.pop_back();
    moved->materials.push_back({"dz\\plants\\tree\\data\\d_piceaabies_stumpb_trunk_a.rvmat",
        "dz\\plants\\tree\\data\\t_piceaabies_trunk_dxt5.paa",
        Poseidon::RapStageCandidateHandoff::Consumer::LayerNormal1, member});
    CHECK(moved->Bounded());
    Poseidon::RapStageCandidateHandoff tooLong;
    tooLong.generation = 7;
    tooLong.materials.push_back({"material.rvmat", std::string(241, 'x'),
        Poseidon::RapStageCandidateHandoff::Consumer::NormalMap, member});
    CHECK_FALSE(tooLong.Bounded());
    moved->materials.pop_back();
    moved->materials[0].sourceMember.bytes = 0;
    CHECK_FALSE(moved->Bounded());
}

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
    std::string buffer(text);
    QIStream in(buffer.data(), static_cast<int>(buffer.size()));
    ParamFile file;
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
    auto material = parse(kSuper);
    const auto* stage = material.FindStage(1);
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

// Corpus witness: point WGR_TEST_RVMAT at a binarised (raP) rvmat and this prints what the raP
// walker reads out of it -- every TexGen's uvSource and every stage's texGen link. Skipped
// when the variable is unset, so it never runs in CI; it exists because the text-config path
// and the raP path are two readers, and MAT-039's mask-on-tex1 case passed the first while the
// game (which takes the second) reported an empty uvSource.
TEST_CASE("RvMaterialSource: raP witness dump", "[asset][material][mat-039][.witness]")
{
    const char* path = std::getenv("WGR_TEST_RVMAT");
    if (!path)
        return;
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto material = Poseidon::Asset::Material::ParseArmaRapMaterial(bytes, path);
    // The stage->TexGen link is what MAT-039 depends on; binarised BI rvmats store `texGen`
    // as text, and a reader that only takes numbers leaves every stage at -1.
    for (const auto& st : material.stages)
        CHECK(st.texGen >= 0);
    WARN("pixelShader=" << material.pixelShaderId << " texGens=" << material.texGens.size()
                        << " stages=" << material.stages.size());
    for (const auto& tg : material.texGens)
        WARN("TexGen" << tg.index << " uvSource='" << tg.uvSource << "' transform=" << tg.uvTransform.present);
    for (const auto& st : material.stages)
        WARN("Stage" << st.index << " texGen=" << st.texGen << " uvSource='" << st.uvSource << "' tex="
                     << st.texture.raw << " transform=" << st.uvTransform.present);
    // Raw member names + types of the first stage body, to see what the walker is looking at.
    const auto root = Poseidon::Asset::Config::ArmaRapReader(bytes).root();
    for (const auto& m : root)
    {
        if (m.type != 0 || m.name.rfind("Stage", 0) != 0)
            continue;
        std::string line = m.name + ":";
        for (const auto& b : m.body)
            line += " " + b.name + "(t" + std::to_string(int(b.type)) + "/v" + std::to_string(int(b.value.type)) + "=" +
                    (b.value.type == 0 || b.value.type == 4 ? b.value.text : std::to_string(b.value.number)) + ")";
        WARN(line);
        break;
    }
}
