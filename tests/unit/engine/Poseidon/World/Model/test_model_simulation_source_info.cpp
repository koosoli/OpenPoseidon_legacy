#include <catch2/catch_test_macros.hpp>
#include "../../../../../../apps/tools/Tools/commands/ModelSimulationSourceInfo.hpp"
#include <limits>

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
