#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/World/Terrain/SimulationResidencyState.hpp>
#include <Poseidon/World/Terrain/SimulationResidencyPolicy.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include "test_fixtures.hpp"
#include <limits>

using namespace Poseidon;
using namespace Poseidon::Streaming;

TEST_CASE("Simulation eligibility rejects recursive animated and unsupported sources before installation", "[streaming-simulation-residency]")
{
    auto model = Asset::Formats::MLODLoader::load(GET_FIXTURE("mlod/p3dm_geometry_mass.p3d"));
    REQUIRE(SimulationSourceAdmissible(model));
    SECTION("unresolved explicit proxy") { model.lodLevels.front().mesh.proxies.emplace_back("unknown"); }
    SECTION("unresolved selection proxy") { model.lodLevels.front().mesh.selections.emplace_back("proxy:unknown.001"); }
    SECTION("skeletal influence despite animation flag")
    {
        model.lodLevels.front().mesh.selections.emplace_back("bone");
        model.lodLevels.front().mesh.selections.back().vertexWeights = {255};
    }
    SECTION("nonbinary source influence")
    {
        model.lodLevels.front().mesh.selections.emplace_back("bone");
        model.lodLevels.front().mesh.selections.back().sourceVertexWeights = {2};
    }
    SECTION("vertex frames") { model.lodLevels.front().mesh.frames.emplace_back(); }
    SECTION("animation policy") { model.allowAnimation = true; }
    SECTION("unsupported native format") { model.sourceFormat = "XOB"; }
    SECTION("nonfinite source geometry")
    { model.lodLevels.front().mesh.vertices.front().position.x = std::numeric_limits<float>::quiet_NaN(); }
    REQUIRE_FALSE(SimulationSourceAdmissible(model));
}

TEST_CASE("Generic simulation routes cannot certify config-backed type substitutions", "[streaming-simulation-residency]")
{
    REQUIRE(SimulationRoute("") == SimulationFactoryRoute::Plain);
    REQUIRE(SimulationRoute("unrecognized") == SimulationFactoryRoute::Plain);
    REQUIRE(SimulationRoute("road") == SimulationFactoryRoute::Road);
    for (auto type : {"house", "forest", "vehicle", "church", "streetlamp"})
        REQUIRE(SimulationRoute(type) == SimulationFactoryRoute::Unsupported);
    // NewObject compares these names case-sensitively too.
    REQUIRE(SimulationRoute("House") == SimulationFactoryRoute::Plain);
}

TEST_CASE("Simulation origin padding covers every exact nonforest placement repair", "[streaming-simulation-residency]")
{
    Matrix4 raw = MIdentity;
    raw.SetDirectionAside(Vector3(2, 1, 0)); raw.SetDirectionUp(Vector3(0, 3, 1));
    raw.SetDirection(Vector3(1, 0, 4)); raw.SetPosition(Vector3(1200, 11.5f, 1200));
    auto rows = SimulationFrameSample(raw);
    const auto maximum = SimulationPlacementMaximumScale(rows);
    REQUIRE(maximum.has_value());
    for (bool network : {false, true}) for (bool keepUp : {false, true})
    {
        const auto repaired = RepairWorldObjectPlacementFrame(raw, false, network, keepUp);
        REQUIRE(double(repaired.Scale()) <= *maximum);
        REQUIRE(SimulationFrameSample(repaired)[9] == rows[9]);
        REQUIRE(SimulationFrameSample(repaired)[10] == rows[10]);
        REQUIRE(SimulationFrameSample(repaired)[11] == rows[11]);
    }
    rows[0] = std::numeric_limits<float>::infinity();
    REQUIRE_FALSE(SimulationPlacementMaximumScale(rows).has_value());
    rows = {}; // Degenerate repaired frames cannot prove a finite rejection envelope.
    REQUIRE_FALSE(SimulationPlacementMaximumScale(rows).has_value());
}

TEST_CASE("Geometry without all query roles remains Unknown rather than ready or empty", "[streaming-simulation-residency]")
{
    Foundation::CaptureMainThread();
    auto model = Asset::Formats::MLODLoader::load(GET_FIXTURE("mlod/p3dm_geometry_mass.p3d"));
    REQUIRE(SimulationSourceAdmissible(model));
    Ref<LODShapeWithShadow> shape = Model::ShapeAdapter::convertToLODShape(model, false);
    REQUIRE(shape);
    REQUIRE(shape->FindGeometryLevel() >= 0);
    REQUIRE_FALSE(SimulationShapeAdmissible(*shape));
    REQUIRE(SimulationShapeElementBytes(*shape) >= sizeof(LODShapeWithShadow));
}

TEST_CASE("Transient simulation replacement protects old geometry until complete and balances shared leases", "[streaming-simulation-residency]")
{
    Foundation::CaptureMainThread();
    SimulationResidencyState state;
    state.leaseCounts = {1, 2, 1, 0};
    SimulationResidencyState::Region region;
    region.active = {0, 1}; region.activeSet = {0, 1};
    region.pending = {1, 2}; region.pendingSet = {1, 2}; region.pendingNew = 1;
    SECTION("cancelled incomplete replacement")
    {
        state.DropPending(region);
        REQUIRE(state.leaseCounts == std::vector<uint32_t>{1, 2, 0, 0});
        REQUIRE(region.active == std::vector<uint32_t>{0, 1});
        REQUIRE(region.pending.empty());
    }
    SECTION("complete replacement releases only retired old objects")
    {
        state.CompleteReplacement(region);
        REQUIRE(region.status == SimulationResidencyStatus::Ready);
        REQUIRE(state.leaseCounts == std::vector<uint32_t>{0, 2, 1, 0});
        REQUIRE(region.active == std::vector<uint32_t>{1, 2});
        state.DropRegion(region);
        REQUIRE(state.leaseCounts == std::vector<uint32_t>{0, 1, 0, 0});
        // Second actor still owns placement1; releasing the first did not evict it.
        REQUIRE(region.active.empty());
    }
    SECTION("dead actor releases old plus unshared pending once")
    {
        state.DropRegion(region);
        REQUIRE(state.leaseCounts == std::vector<uint32_t>{0, 1, 0, 0});
        state.DropRegion(region);
        REQUIRE(state.leaseCounts == std::vector<uint32_t>{0, 1, 0, 0});
    }
}

