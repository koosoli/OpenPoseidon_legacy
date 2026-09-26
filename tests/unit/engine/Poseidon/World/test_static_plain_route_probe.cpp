#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/StaticPlainRouteProbe.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/IO/ParamFile/InitLibraryElement.hpp>
#include <Poseidon/IO/ParamFile/ParamFile.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include <string>
#include <thread>

using namespace Poseidon;
using namespace Poseidon::Streaming;
namespace
{
using Result = StaticPlainRouteObservation;
Model::Model OriginalOdol()
{
    Model::Model model;
    model.sourceFormat = "ODOL";
    model.sourceVersion = 54;
    // Original fixture declares all four source LODs and has no hidden pose data.
    model.sourceAudit.producerVersion = 1;
    model.sourceAudit.sourceRevision = 54;
    model.sourceAudit.geometryCoverage = Model::SourceGeometryCoverage::DeclaredLodsDecoded;
    model.sourceAudit.observations = Model::SourceAuditAllObservations;
    model.sourceAudit.declaredLods = model.sourceAudit.decodedLods = 4;
    model.sourcePath = "dz\\structures\\wall.p3d";
    model.boundingSphere.radius = 2;
    for (float resolution : {1.f, 1e13f, 7e15f, 6e15f})
    {
        Model::LODLevel lod(resolution);
        for (const auto& position : {Model::Vector3{-1, -1, -1}, Model::Vector3{1, -1, 1}, Model::Vector3{1, 1, -1}})
        {
            Model::Vertex vertex;
            vertex.position = position;
            lod.mesh.vertices.push_back(vertex);
        }
        lod.mesh.triangles.emplace_back(0, 1, 2);
        model.lodLevels.push_back(std::move(lod));
    }
    return model;
}
void ParseOriginal(ParamFile& root, const std::string& text)
{
    InitLibraryElement();
    QIStream input(text.data(), static_cast<int>(text.size()));
    root.Parse(input);
}
Result Probe(const Model::Model& model, const ParamEntry& root)
{
    Foundation::CaptureMainThread();
    const auto evidence = BuildStaticSourceEnvelope(model, 17, StaticSourceCoverage::FullCompiledIR);
    REQUIRE(evidence.State() == StaticSourceEnvelopeState::SourceEvidence);
    const auto result = ProbeStaticPlainRouteNow(model, evidence, 17, root);
    const auto summary = BuildStaticPlainSourceSummary(model, evidence);
    REQUIRE(ProbeStaticPlainRouteNow(summary, 17, model.sourcePath, root) == result);
    return result;
}
}

TEST_CASE("Live empty-class probe reads parsed local CfgModels without default fallback", "[streaming-static-plain-route]")
{
    auto model = OriginalOdol();
    ParamFile root;
    SECTION("no CfgModels") { ParseOriginal(root, "class Other { value=1; };"); }
    SECTION("empty CfgModels") { ParseOriginal(root, "class CfgModels {};"); }
    SECTION("unmatched default does not force class")
    { ParseOriginal(root, "class CfgModels { class Default { properties[]={\"class\",\"house\"}; }; };"); }
    SECTION("unmatched model")
    { ParseOriginal(root, "class CfgModels { class Another { properties[]={\"class\",\"house\"}; }; };"); }
    SECTION("empty class and other property")
    { ParseOriginal(root, "class CfgModels { class Wall { properties[]={\"class\",\"\",\"map\",\"wall\"}; }; };"); }
    REQUIRE(Probe(model, root) == Result::EmptyClassNow);
}

TEST_CASE("Live class overrides and every source LOD conservatively refuse special routes", "[streaming-static-plain-route]")
{
    auto model = OriginalOdol();
    ParamFile root;
    SECTION("forced house")
    { ParseOriginal(root, "class CfgModels { class Wall { properties[]={\"class\",\"house\"}; }; };"); }
    SECTION("forced uppercase class")
    { ParseOriginal(root, "class CfgModels { class Wall { properties[]={\"CLASS\",\"HOUSE\"}; }; };"); }
    SECTION("later blank cannot hide a forced route")
    { ParseOriginal(root, "class CfgModels { class Wall { properties[]={\"class\",\"house\",\"class\",\"\"}; }; };"); }
    SECTION("unselected LOD contains class")
    {
        ParseOriginal(root, "class CfgModels {};");
        model.lodLevels.back().mesh.properties.emplace_back("CLASS", "HOUSE");
    }
    SECTION("later duplicate source blank")
    {
        ParseOriginal(root, "class CfgModels {};");
        model.lodLevels[0].mesh.properties.emplace_back("class", "house");
        model.lodLevels[0].mesh.properties.emplace_back("class", "");
    }
    REQUIRE(Probe(model, root) == Result::Unsupported);
}

TEST_CASE("Probe uses exact legacy basename normalization including first dot", "[streaming-static-plain-route]")
{
    auto model = OriginalOdol();
    model.sourcePath = "dir/sub\\Wall - (A).extra.p3d";
    ParamFile root;
    ParseOriginal(root, "class cfgmodels { class WALL____A_ { properties[]={\"class\",\"house\"}; }; };");
    REQUIRE(Probe(model, root) == Result::Unsupported);
    ParamFile differentRoot;
    ParseOriginal(differentRoot, "class CfgModels { class Wall____A__extra { properties[]={\"class\",\"house\"}; }; };");
    REQUIRE(Probe(model, differentRoot) == Result::EmptyClassNow);
}

TEST_CASE("Probe refuses ShapeBank path truncation before another basename can force class", "[streaming-static-plain-route]")
{
    auto model = OriginalOdol();
    ParamFile root;
    ParseOriginal(root, "class CfgModels { class Forced { properties[]={\"class\",\"house\"}; }; };");
    // Full input names Wall; the real factory's first 127 characters instead
    // name Forced. A lookup against Wall would incorrectly report empty class.
    model.sourcePath = std::string(120, 'd') + "/ForcedSuffix/Wall.p3d";
    const std::string truncated = model.sourcePath.substr(0, 127);
    REQUIRE(truncated.substr(truncated.find_last_of('/') + 1) == "Forced");
    REQUIRE(Probe(model, root) == Result::Unsupported);
    // The truncated path itself also observes the actual parsed forced class.
    model.sourcePath = truncated;
    REQUIRE(model.sourcePath.size() == 127);
    REQUIRE(Probe(model, root) == Result::Unsupported);
}

TEST_CASE("Probe accepts the last untruncated path byte and refuses the next", "[streaming-static-plain-route]")
{
    auto model = OriginalOdol();
    ParamFile root;
    ParseOriginal(root, "class CfgModels { class Wall { properties[]={\"class\",\"\"}; }; };");
    model.sourcePath = std::string(118, 'd') + "/Wall.p3d";
    REQUIRE(model.sourcePath.size() == 127);
    REQUIRE(Probe(model, root) == Result::EmptyClassNow);
    model.sourcePath.insert(0, "d");
    REQUIRE(model.sourcePath.size() == 128);
    REQUIRE(Probe(model, root) == Result::Unsupported);
}

TEST_CASE("Inherited and malformed parsed config cannot produce empty-class observation", "[streaming-static-plain-route]")
{
    auto model = OriginalOdol();
    ParamFile root;
    SECTION("CfgModels inherited")
    { ParseOriginal(root, "class Base {}; class CfgModels:Base {};"); }
    SECTION("model inherited")
    { ParseOriginal(root, "class CfgModels { class Base { properties[]={\"class\",\"house\"}; }; class Wall:Base {}; };"); }
    SECTION("model nonclass") { ParseOriginal(root, "class CfgModels { wall=1; };"); }
    SECTION("CfgModels nonclass") { ParseOriginal(root, "CfgModels=1;"); }
    SECTION("scalar properties") { ParseOriginal(root, "class CfgModels { class Wall { properties=1; }; };"); }
    SECTION("odd pairs") { ParseOriginal(root, "class CfgModels { class Wall { properties[]={\"class\"}; }; };"); }
    SECTION("numeric element") { ParseOriginal(root, "class CfgModels { class Wall { properties[]={\"class\",0}; }; };"); }
    SECTION("quoted numeric is parsed as numeric")
    { ParseOriginal(root, "class CfgModels { class Wall { properties[]={\"class\",\"\",\"armor\",\"5\"}; }; };"); }
    SECTION("empty key") { ParseOriginal(root, "class CfgModels { class Wall { properties[]={\"\",\"\"}; }; };"); }
    SECTION("nested array element") { ParseOriginal(root, "class CfgModels { class Wall { properties[]={\"class\",{}}; }; };"); }
    REQUIRE(Probe(model, root) == Result::Unsupported);
}

TEST_CASE("Unnamed config bases cannot hide inheritance from the local-only probe", "[streaming-static-plain-route]")
{
    auto model = OriginalOdol();
    ParamFile base;
    ParseOriginal(base, "class CfgModels { class Wall { properties[]={\"class\",\"house\"}; }; };");
    ParamFile root;
    ParseOriginal(root, "class CfgModels { class Wall { properties[]={\"class\",\"\"}; }; };");
    REQUIRE_FALSE(root.HasBase());
    REQUIRE_FALSE(root.FindEntry("CfgModels")->GetClassInterface()->HasBase());
    REQUIRE_FALSE(root.FindEntry("CfgModels")->FindEntry("Wall")->GetClassInterface()->HasBase());
    REQUIRE(Probe(model, root) == Result::EmptyClassNow);
    SECTION("injected root has unnamed base")
    {
        root.SetBase(&base);
        REQUIRE(root.HasBase());
    }
    SECTION("CfgModels has unnamed base")
    {
        auto* models = root.FindEntry("CfgModels")->GetClassInterface();
        REQUIRE(models);
        models->SetBase(&base);
        REQUIRE(models->HasBase());
    }
    SECTION("model has unnamed base")
    {
        auto* wall = root.FindEntry("CfgModels")->FindEntry("Wall")->GetClassInterface();
        REQUIRE(wall);
        wall->SetBase(&base);
        REQUIRE(wall->HasBase());
    }
    REQUIRE(Probe(model, root) == Result::Unsupported);
}

TEST_CASE("Probe bounds source and visible config text before using it", "[streaming-static-plain-route]")
{
    auto model = OriginalOdol();
    ParamFile root;
    ParseOriginal(root, "class CfgModels {};");
    SECTION("source embedded NUL name")
    { model.lodLevels[0].mesh.properties.emplace_back(std::string("class\0suffix", 12), ""); }
    SECTION("source embedded NUL value")
    { model.lodLevels[0].mesh.properties.emplace_back("class", std::string("\0house", 6)); }
    SECTION("source nonASCII") { model.lodLevels[0].mesh.properties.emplace_back("map", std::string(1, char(0x80))); }
    SECTION("source oversized property")
    { model.lodLevels[0].mesh.properties.emplace_back("map", std::string(StaticPlainRouteLimits::StringBytes + 1, 'a')); }
    SECTION("source property cap")
    { model.lodLevels[0].mesh.properties.resize(StaticPlainRouteLimits::SourceProperties + 1, Model::NamedProperty("map", "")); }
    SECTION("basename exceeds legacy buffer") { model.sourcePath = std::string(256, 'a'); }
    SECTION("path cap") { model.sourcePath = std::string(StaticPlainRouteLimits::PathBytes + 1, 'a'); }
    SECTION("path embedded NUL") { model.sourcePath = std::string("wall\0.p3d", 9); }
    SECTION("empty path") { model.sourcePath.clear(); }
    SECTION("config text oversized")
    { ParseOriginal(root, "class CfgModels { class Wall { properties[]={\"map\",\"" + std::string(257, 'a') + "\"}; }; };"); }
    SECTION("config nonASCII text")
    { ParseOriginal(root, "class CfgModels { class Wall { properties[]={\"map\",\"" + std::string(1, char(0x80)) + "\"}; }; };"); }
    REQUIRE(Probe(model, root) == Result::Unsupported);
}

TEST_CASE("Manual local config scans and property arrays have hard admission caps", "[streaming-static-plain-route]")
{
    auto model = OriginalOdol();
    ParamFile root;
    std::string text;
    SECTION("root cap")
    {
        for (int i = 0; i <= StaticPlainRouteLimits::RootEntries; ++i) text += "value" + std::to_string(i) + "=1;";
    }
    SECTION("CfgModels cap")
    {
        text = "class CfgModels {";
        for (int i = 0; i <= StaticPlainRouteLimits::ModelEntries; ++i) text += "class model" + std::to_string(i) + "{};";
        text += "};";
    }
    SECTION("model override entry cap")
    {
        text = "class CfgModels { class Wall {";
        for (int i = 0; i <= StaticPlainRouteLimits::OverrideEntries; ++i) text += "value" + std::to_string(i) + "=1;";
        text += "}; };";
    }
    SECTION("properties element cap")
    {
        text = "class CfgModels { class Wall { properties[]={";
        for (int i = 0; i < StaticPlainRouteLimits::PropertyElements + 2; ++i) text += (i ? ",\"map\"" : "\"map\"");
        text += "}; }; };";
    }
    ParseOriginal(root, text);
    REQUIRE(Probe(model, root) == Result::Unsupported);
}

TEST_CASE("Empty-class observation is refreshed from actual live config mutation", "[streaming-static-plain-route]")
{
    auto model = OriginalOdol();
    ParamFile root;
    ParseOriginal(root, "class CfgModels { class Wall { properties[]={\"class\",\"\"}; }; };");
    const auto summary = BuildStaticPlainSourceSummary(model,
        BuildStaticSourceEnvelope(model, 17, StaticSourceCoverage::FullCompiledIR));
    REQUIRE(Probe(model, root) == Result::EmptyClassNow);
    REQUIRE(ProbeStaticPlainRouteNow(summary, 17, model.sourcePath, root) == Result::EmptyClassNow);
    auto* models = root.FindEntry("CfgModels");
    REQUIRE(models);
    auto* wall = models->FindEntry("Wall");
    REQUIRE(wall);
    auto* properties = wall->FindEntry("properties");
    REQUIRE(properties);
    properties->SetValue(1, Foundation::RStringB("house"));
    REQUIRE(Probe(model, root) == Result::Unsupported);
    REQUIRE(ProbeStaticPlainRouteNow(summary, 17, model.sourcePath, root) == Result::Unsupported);
    properties->SetValue(1, Foundation::RStringB(""));
    REQUIRE(Probe(model, root) == Result::EmptyClassNow);
    REQUIRE(ProbeStaticPlainRouteNow(summary, 17, model.sourcePath, root) == Result::EmptyClassNow);
}

TEST_CASE("Probe rejects stale partial or unsupported evidence and wrong owner", "[streaming-static-plain-route]")
{
    Foundation::CaptureMainThread();
    const auto model = OriginalOdol();
    ParamFile root;
    ParseOriginal(root, "class CfgModels {};");
    const auto evidence = BuildStaticSourceEnvelope(model, 17, StaticSourceCoverage::FullCompiledIR);
    REQUIRE(ProbeStaticPlainRouteNow(model, evidence, 18, root) == Result::StaleSource);
    REQUIRE(ProbeStaticPlainRouteNow(model, evidence, 0, root) == Result::StaleSource);
    const auto partial = BuildStaticSourceEnvelope(model, 17, StaticSourceCoverage::Partial);
    REQUIRE(ProbeStaticPlainRouteNow(model, partial, 17, root) == Result::Unknown);
    auto unsupported = model;
    unsupported.sourceFormat = "MLOD";
    const auto mlod = BuildStaticSourceEnvelope(unsupported, 17, StaticSourceCoverage::FullCompiledIR);
    REQUIRE(ProbeStaticPlainRouteNow(unsupported, mlod, 17, root) == Result::Unknown);
    Result observed = Result::Unknown;
    std::thread([&] { observed = ProbeStaticPlainRouteNow(model, evidence, 17, root); }).join();
    REQUIRE(observed == Result::WrongThread);
}
