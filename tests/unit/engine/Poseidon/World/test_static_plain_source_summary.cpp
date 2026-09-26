#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/StaticPlainRouteProbe.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/IO/ParamFile/InitLibraryElement.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <memory>
#include <thread>
#include <type_traits>

using namespace Poseidon;
using namespace Poseidon::Streaming;
namespace
{
using State = StaticPlainSourceSummaryState;
using Result = StaticPlainRouteObservation;
Model::Model OriginalSummarySource()
{
    Model::Model model;
    model.sourceFormat="ODOL"; model.sourceVersion=7;
    model.sourcePath="dir/sub\\Wall - (A).extra.p3d";
    model.sourceAudit.producerVersion=1; model.sourceAudit.sourceRevision=7;
    model.sourceAudit.geometryCoverage=Model::SourceGeometryCoverage::DeclaredLodsDecoded;
    model.sourceAudit.observations=Model::SourceAuditAllObservations;
    model.sourceAudit.declaredLods=model.sourceAudit.decodedLods=4;
    model.boundingSphere.radius=2;
    for(float resolution : {1.f,1e13f,7e15f,6e15f})
    {
        Model::LODLevel lod(resolution);
        for(Model::Vector3 position : {Model::Vector3{0,0,0},Model::Vector3{1,0,0},Model::Vector3{0,1,0}})
        { Model::Vertex vertex; vertex.position=position; lod.mesh.vertices.push_back(vertex); }
        lod.mesh.triangles.emplace_back(0,1,2);
        model.lodLevels.push_back(std::move(lod));
    }
    return model;
}
StaticPlainSourceSummary Summary(const Model::Model& model)
{
    return BuildStaticPlainSourceSummary(model,
        BuildStaticSourceEnvelope(model,17,StaticSourceCoverage::FullCompiledIR));
}
void ParseOriginal(ParamFile& root, const std::string& text)
{
    InitLibraryElement(); QIStream input(text.data(),int(text.size())); root.Parse(input);
}
}

TEST_CASE("Compact summary extracts exclusive worker source and owns names after IR destruction", "[streaming-static-source-summary]")
{
    static_assert(std::is_trivially_copyable_v<StaticPlainSourceSummary>);
    static_assert(sizeof(StaticPlainSourceSummary)<=288);
    Foundation::CaptureMainThread();
    StaticPlainSourceSummary summary;
    std::thread([source=std::make_unique<Model::Model>(OriginalSummarySource()),&summary]
    { summary=Summary(*source); }).join(); // unique source destroyed by worker
    REQUIRE(summary.State()==State::SourceFacts);
    REQUIRE(summary.SourceGeneration()==17);
    REQUIRE(summary.ModelIdentity()=="dir/sub\\Wall - (A).extra.p3d");
    REQUIRE(summary.ConfigModelName()=="Wall____A_");
    const auto copied=summary;
    summary={};
    REQUIRE(copied.ModelIdentity()=="dir/sub\\Wall - (A).extra.p3d");
    REQUIRE(copied.ConfigModelName()=="Wall____A_");
    ParamFile root;
    ParseOriginal(root,"class CfgModels { class WALL____A_ { properties[]={\"class\",\"house\"}; }; };");
    REQUIRE(ProbeStaticPlainRouteNow(copied,17,copied.ModelIdentity(),root)==Result::Unsupported);
}

TEST_CASE("Summary probe requires exact inventory identity generation and live owner", "[streaming-static-source-summary]")
{
    Foundation::CaptureMainThread();
    const auto summary=Summary(OriginalSummarySource());
    REQUIRE(summary.State()==State::SourceFacts);
    ParamFile root; ParseOriginal(root,"class CfgModels {};");
    REQUIRE(ProbeStaticPlainRouteNow(summary,17,summary.ModelIdentity(),root)==Result::EmptyClassNow);
    REQUIRE(ProbeStaticPlainRouteNow(summary,0,summary.ModelIdentity(),root)==Result::StaleSource);
    REQUIRE(ProbeStaticPlainRouteNow(summary,18,summary.ModelIdentity(),root)==Result::StaleSource);
    REQUIRE(ProbeStaticPlainRouteNow(summary,17,"dir/sub\\wall - (A).extra.p3d",root)==Result::StaleSource);
    REQUIRE(ProbeStaticPlainRouteNow(summary,17,"dir/sub/Wall - (A).extra.p3d",root)==Result::StaleSource);
    Result result=Result::Unknown;
    std::thread([&]{result=ProbeStaticPlainRouteNow(summary,17,summary.ModelIdentity(),root);}).join();
    REQUIRE(result==Result::WrongThread);
}

TEST_CASE("Summary source audit and capped facts fail closed without retaining partial names", "[streaming-static-source-summary]")
{
    auto model=OriginalSummarySource();
    SECTION("class on final source LOD") { model.lodLevels.back().mesh.properties.emplace_back("CLASS","house"); }
    SECTION("embedded NUL property") { model.lodLevels[0].mesh.properties.emplace_back("map",std::string("wall\0x",6)); }
    SECTION("property cap") { model.lodLevels[0].mesh.properties.resize(StaticPlainRouteLimits::SourceProperties+1,Model::NamedProperty("map","")); }
    SECTION("factory truncation") { model.sourcePath=std::string(128,'a'); }
    SECTION("embedded NUL path") { model.sourcePath=std::string("wall\0.p3d",9); }
    const auto summary=Summary(model);
    REQUIRE(summary.State()==State::Unsupported);
    REQUIRE(summary.ModelIdentity().empty());
    REQUIRE(summary.ConfigModelName().empty());
}

TEST_CASE("Missing source audit and partial envelope cannot become source summary facts", "[streaming-static-source-summary]")
{
    auto source=OriginalSummarySource();
    auto envelope=BuildStaticSourceEnvelope(source,17,StaticSourceCoverage::FullCompiledIR);
    REQUIRE(envelope.State()==StaticSourceEnvelopeState::SourceEvidence);
    SECTION("old unaudited IR") { source.sourceAudit={}; }
    SECTION("revision mismatch") { source.sourceAudit.sourceRevision=54; }
    SECTION("partial envelope") { envelope=BuildStaticSourceEnvelope(source,17,StaticSourceCoverage::Partial); }
    const auto summary=BuildStaticPlainSourceSummary(source,envelope);
    REQUIRE(summary.State()==State::Unknown);
    REQUIRE(summary.ModelIdentity().empty());
}
