#include <catch2/catch_test_macros.hpp>
#include "../../../../../../apps/tools/Tools/commands/ModelSimulationSourceInfo.hpp"
#include <limits>

namespace
{
Poseidon::Model::Model OriginalStaticDiagnosticSource()
{
    namespace IR = Poseidon::Model;
    IR::Model model;
    model.sourcePath="original/wall.p3d"; model.sourceFormat="ODOL"; model.sourceVersion=54;
    model.boundingSphere.radius=2;
    auto& audit=model.sourceAudit;
    audit.producerVersion=1; audit.sourceRevision=54;
    audit.geometryCoverage=IR::SourceGeometryCoverage::DeclaredLodsDecoded;
    audit.observations=IR::SourceAuditAllObservations;
    audit.declaredLods=audit.decodedLods=4;
    for (float resolution : {1.f,1e13f,7e15f,6e15f})
    {
        IR::LODLevel lod(resolution);
        for (IR::Vector3 position : {IR::Vector3{0,0,0},IR::Vector3{1,0,0},IR::Vector3{0,1,0}})
        { IR::Vertex vertex; vertex.position=position; lod.mesh.vertices.push_back(vertex); }
        lod.mesh.triangles.emplace_back(0,1,2);
        model.lodLevels.push_back(std::move(lod));
    }
    return model;
}
}

TEST_CASE("Raw simulation source report retains authored properties and unsafe animation facts", "[tools][simulation-source-info]")
{
    Poseidon::Model::Model model;
    model.sourcePath = "wall.p3d"; model.sourceFormat = "ODOL"; model.sourceVersion = 54;
    model.allowAnimation = 1;
    model.lodLevels.emplace_back(1e13f);
    auto& mesh = model.lodLevels.back().mesh;
    mesh.properties.emplace_back("class", "house");
    mesh.properties.emplace_back("animated", "true");
    mesh.frames.emplace_back(); mesh.proxies.emplace_back("proxy:door");
    mesh.selections.emplace_back();
    auto& selection = mesh.selections.back();
    selection.name = "proxy:door"; selection.vertexWeights = {255}; selection.sourceVertexWeights = {1, 0, 2};
    mesh.vertices.emplace_back();
    mesh.vertices.back().position.x = std::numeric_limits<float>::quiet_NaN();
    const auto output = PoseidonTools::FormatModelSimulationSourceInfo(model);
    CHECK(output.find("Format: \"ODOL\" version=54") != std::string::npos);
    CHECK(output.find("allowAnimation: 1") != std::string::npos);
    CHECK(output.find("purpose=Geometry") != std::string::npos);
    CHECK(output.find("name=\"class\" value=\"house\"") != std::string::npos);
    CHECK(output.find("name=\"animated\" value=\"true\"") != std::string::npos);
    CHECK(output.find("proxies=1 frames=1 selections=1 proxySelections=1 weightedSelections=1 nonUnitSourceWeights=2") != std::string::npos);
    CHECK(output.find("nonfinitePositions=1") != std::string::npos);
    CHECK(output.find("source completeness is not certified") != std::string::npos);
    CHECK(output.find("Source audit: producer=0 revision=0 geometryCoverage=0 observations=0") != std::string::npos);
    CHECK(output.find("No Shape adaptation, constructor/config resolution, or simulation Ready certification") != std::string::npos);
    CHECK(output.find("Source envelope: state=Unknown") != std::string::npos);
    CHECK(output.find("IncompleteSource") != std::string::npos);
    CHECK(output.find("Plain source summary: state=Unknown") != std::string::npos);
}

TEST_CASE("Source report exposes discarded authored motion independently of empty IR frames", "[tools][simulation-source-info]")
{
    Poseidon::Model::Model model;
    model.sourceFormat = "ODOL"; model.sourceVersion = 54;
    auto& audit = model.sourceAudit;
    audit.producerVersion = 1; audit.sourceRevision = 54;
    audit.geometryCoverage = Poseidon::Model::SourceGeometryCoverage::DeclaredLodsDecoded;
    audit.observations = Poseidon::Model::SourceAuditAllObservations;
    audit.declaredLods = audit.decodedLods = 1;
    audit.keyframeCount = 2; audit.keyframePayloadDiscarded = true;
    model.lodLevels.emplace_back(1.f);
    const auto output = PoseidonTools::FormatModelSimulationSourceInfo(model);
    CHECK(output.find("keyframes=2 keyframePayloadDiscarded=1") != std::string::npos);
    CHECK(output.find("proxies=0 frames=0") != std::string::npos);
    CHECK(output.find("observations=15 declaredLods=1 decodedLods=1") != std::string::npos);
}

TEST_CASE("Raw simulation source report bounds rows and strings and escapes embedded control bytes", "[tools][simulation-source-info]")
{
    Poseidon::Model::Model model;
    model.lodLevels.resize(PoseidonTools::SimulationInfoMaxLods + 2);
    auto& properties = model.lodLevels.front().mesh.properties;
    properties.resize(PoseidonTools::SimulationInfoMaxProperties + 3);
    properties[0].name = "class\n\"\\";
    properties[0].value.assign(PoseidonTools::SimulationInfoMaxStringBytes + 7, 'x');
    properties.back().name = "property-must-be-omitted";
    const auto output = PoseidonTools::FormatModelSimulationSourceInfo(model);
    CHECK(output.find("omittedLods=2") != std::string::npos);
    CHECK(output.find("omittedProperties=3") != std::string::npos);
    CHECK(output.find("omittedBytes=7") != std::string::npos);
    CHECK(output.find("class\\x0a\\\"\\\\") != std::string::npos);
    CHECK(output.find("property-must-be-omitted") == std::string::npos);
    CHECK(output.size() < 1024 * 1024);
}

TEST_CASE("Explicit offline report exposes audited source facts with synthetic identity scope", "[tools][simulation-source-info]")
{
    const auto output=PoseidonTools::FormatModelSimulationSourceInfo(OriginalStaticDiagnosticSource());
    CHECK(output.find("generation=1 (synthetic; not world/inventory identity or content freshness)")!=std::string::npos);
    CHECK(output.find("no live config verdict, constructor, final bounds or Ready proof")!=std::string::npos);
    CHECK(output.find("extracted disk paths are not resolved to world inventory names")!=std::string::npos);
    CHECK(output.find("Source envelope: state=SourceEvidence")!=std::string::npos);
    CHECK(output.find("reasons=[] sourceRadius=")!=std::string::npos);
    CHECK(output.find("Plain source summary: state=SourceFacts reason=none identity=\"original/wall.p3d\" configModelName=\"wall\"")!=std::string::npos);
}

TEST_CASE("Offline source envelope refuses missing audit and unsafe geometry with explicit reasons", "[tools][simulation-source-info]")
{
    auto model=OriginalStaticDiagnosticSource();
    std::string reason;
    SECTION("missing audit") { model.sourceAudit={}; reason="IncompleteSource"; }
    SECTION("revision mismatch") { model.sourceAudit.sourceRevision=7; reason="IncompleteSource"; }
    SECTION("authored animation") { model.allowAnimation=1; reason="Animation"; }
    SECTION("proxy tail") { model.lodLevels[0].mesh.proxies.emplace_back("proxy:door"); reason="Proxy"; }
    SECTION("nonfinite position") { model.lodLevels[0].mesh.vertices[0].position.x=std::numeric_limits<float>::quiet_NaN(); reason="NonFinite"; }
    SECTION("invalid face") { model.lodLevels[0].mesh.triangles[0].indices[0]=99; reason="InvalidFace"; }
    SECTION("missing explicit view role")
    {
        model.lodLevels.back().mesh.triangles.clear(); reason="MissingRole";
    }
    const auto output=PoseidonTools::FormatModelSimulationSourceInfo(model);
    REQUIRE_FALSE(reason.empty());
    CHECK(output.find("Source envelope: state=Unknown")!=std::string::npos);
    const auto reasons=output.substr(output.find("reasons=["));
    CHECK(reasons.find(reason)!=std::string::npos);
    CHECK(output.find("Plain source summary: state=Unknown")!=std::string::npos);
    CHECK(output.find("sourceRadius=")==std::string::npos);
}

TEST_CASE("Offline summary refuses source class and supplied path policy without a config verdict", "[tools][simulation-source-info]")
{
    auto model=OriginalStaticDiagnosticSource();
    SECTION("source class on final LOD") { model.lodLevels.back().mesh.properties.emplace_back("CLASS","house"); }
    SECTION("extracted path meets factory truncation boundary") { model.sourcePath=std::string(128,'a'); }
    SECTION("embedded NUL property") { model.lodLevels[0].mesh.properties.emplace_back("map",std::string("wall\0suffix",11)); }
    const auto output=PoseidonTools::FormatModelSimulationSourceInfo(model);
    CHECK(output.find("Source envelope: state=SourceEvidence")!=std::string::npos);
    CHECK(output.find("Plain source summary: state=Unsupported reason=source-path-or-property-policy identity=\"\" configModelName=\"\"")!=std::string::npos);
    CHECK(output.find("EmptyClassNow")==std::string::npos);
    CHECK(output.size()<16*1024);
}
