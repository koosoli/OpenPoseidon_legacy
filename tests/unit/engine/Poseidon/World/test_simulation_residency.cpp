#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/World/Terrain/SimulationResidencyState.hpp>
#include <Poseidon/World/Terrain/SimulationResidencyPolicy.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include "test_fixtures.hpp"
#include <limits>
#include <cstddef>
#include <new>

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


TEST_CASE("Exact-query reuse proves convex capsule containment and refuses diagonal AABB corners", "[streaming-simulation-residency]")
{
    const CoveragePoint from{0, 0, 0}, to{10, 0, 10};
    REQUIRE(SimulationCapsuleContains(from, to, 1, from, to, 1));
    REQUIRE(SimulationCapsuleContains(from, to, 1, to, from, 1));
    REQUIRE(SimulationCapsuleContains(from, to, 1, {1, 0, 1}, {9, 0, 9}, .5));
    REQUIRE(SimulationCapsuleContains(to, from, 1, {9, 0, 9}, {1, 0, 1}, .5));
    // Both rays have identical endpoint AABBs. The crossed diagonal was never
    // covered by the source demand's narrow origin-cell capsule.
    REQUIRE_FALSE(SimulationCapsuleContains(from, to, 1, {0, 0, 10}, {10, 0, 0}, 0));
    REQUIRE_FALSE(SimulationCapsuleContains(from, to, 1, {1, 2, 1}, {9, 2, 9}, 0));
    REQUIRE_FALSE(SimulationCapsuleContains(from, to, 1, {1, 0, 1}, {9, 0, 9}, 2));
    REQUIRE_FALSE(SimulationCapsuleContains(from, to, 1, {1, 0, 1}, {11.01, 0, 11.01}, 0));
}

TEST_CASE("Exact-query containment refuses uncertain boundaries and unsafe numeric domains", "[streaming-simulation-residency]")
{
    const CoveragePoint point{100, 20, 100};
    REQUIRE(SimulationCapsuleContains(point, point, 10, {99, 20, 99}, {101, 20, 101}, 1));
    REQUIRE_FALSE(SimulationCapsuleContains(point, point, 10, {100, 20, 100}, {110, 20, 100}, 0));
    REQUIRE(SimulationCapsuleContains(point, point, 0, point, point, 0));
    REQUIRE_FALSE(SimulationCapsuleContains(point, point, 0, {100.01, 20, 100}, point, 0));
    const CoveragePoint large{1e8, 1e8, 1e8};
    REQUIRE(SimulationCapsuleContains(large, large, 1000, large, {1e8 + 1, 1e8 + 1, 1e8 + 1}, 1));
    REQUIRE_FALSE(SimulationCapsuleContains(large, large, 1000, large, {1e8 + 999, 1e8, 1e8}, 0));
    auto invalid = point;
    invalid[0] = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(SimulationCapsuleContains(point, point, 10, invalid, point, 0));
    REQUIRE_FALSE(SimulationCapsuleContains(point, point, 10, point, point, -1));
    REQUIRE_FALSE(SimulationCapsuleContains(point, point, std::numeric_limits<double>::infinity(), point, point, 0));
    REQUIRE_FALSE(SimulationCapsuleContains(point, point, EngineBroadphaseSafeMagnitude() * 2, point, point, 0));
}

TEST_CASE("Fire query channel cannot override automatic neighborhood or inherit its Ready state", "[streaming-simulation-residency]")
{
    Foundation::CaptureMainThread();
    Ref<LODShapeWithShadow> shape = new LODShapeWithShadow;
    Ref<ObjectPlain> actor = new ObjectPlain(shape, 42);
    SimulationResidencyState state;
    state.leaseCounts = {1, 1};
    const SimulationResidencyState::RegionKey neighborhood{actor, 0};
    const SimulationResidencyState::RegionKey fire{actor, uint8_t(SimulationQueryPurpose::Fire)};
    auto& local = state.regions[neighborhood]; local.actor = actor.GetRef();
    local.automatic = true; local.active = {0}; local.activeSet = {0}; local.status = SimulationResidencyStatus::Ready;
    auto& query = state.regions[fire]; query.actor = actor.GetRef();
    query.active = {1}; query.activeSet = {1};
    REQUIRE(state.regions.size() == 2);
    REQUIRE(state.regions.at(fire).status == SimulationResidencyStatus::Pending);
    REQUIRE(state.regions.at(neighborhood).status == SimulationResidencyStatus::Ready);
    state.DropRegion(state.regions.at(fire)); state.regions.erase(fire);
    REQUIRE(state.leaseCounts == std::vector<uint32_t>{1, 0});
    REQUIRE(state.regions.at(neighborhood).status == SimulationResidencyStatus::Ready);
    REQUIRE(state.regions.at(neighborhood).automatic);
    actor = nullptr;
    REQUIRE(static_cast<Object*>(state.regions.at(neighborhood).actor) == nullptr);
    // Weak ownership does not retain an actor solely because a query is live.
}

TEST_CASE("Live modern placement watcher refuses movement or mutation outside previous demand bins", "[streaming-simulation-residency]")
{
    Foundation::CaptureMainThread();
    auto model = Asset::Formats::MLODLoader::load(GET_FIXTURE("mlod/p3dm_geometry_mass.p3d"));
    Ref<LODShapeWithShadow> shape = Model::ShapeAdapter::convertToLODShape(model, false);
    Ref<ObjectPlain> wall = new ObjectPlain(shape, 1002);
    wall->SetType(Primary);
    Matrix4 frame = MIdentity; frame.SetPosition(Vector3(1230, 11.5f, 1200)); wall->SetTransform(frame);
    SimulationResidencyState::Watch watch;
    watch.object = wall.GetRef(); watch.shape = shape.GetRef(); watch.frame = SimulationFrameSample(frame);
    watch.id = 1002; watch.type = Primary; watch.radius = wall->GetRadius(); watch.authored = true;
    REQUIRE(watch.MatchesImmutableObject(shape, SimulationFactoryRoute::Plain));
    SECTION("moves into distant previously ready region")
    {
        wall->SetPosition(Vector3(100, 11.5f, 100));
        REQUIRE_FALSE(watch.MatchesImmutableObject(shape, SimulationFactoryRoute::Plain));
    }
    SECTION("mutated persistent state at lease0")
    {
        wall->SetDammage(.25f);
        REQUIRE(wall->MustBeSaved());
        REQUIRE_FALSE(watch.MatchesImmutableObject(shape, SimulationFactoryRoute::Plain));
    }
    SECTION("factory type mismatch")
    {
        wall->SetType(Network);
        REQUIRE_FALSE(watch.MatchesImmutableObject(shape, SimulationFactoryRoute::Plain));
    }
    SECTION("unproven authored frame cannot become a current certificate")
    {
        watch.authored = false;
        REQUIRE_FALSE(watch.MatchesImmutableObject(shape, SimulationFactoryRoute::Plain));
    }
}

namespace
{
class PlacementSimulationActor
{
    alignas(ObjectPlain) std::byte _bytes[sizeof(ObjectPlain)];
    ObjectPlain* _object = nullptr;
public:
    ~PlacementSimulationActor() { Reset(); }
    void Reset() { if (_object) { _object->~ObjectPlain(); _object = nullptr; } }
    ObjectPlain& Emplace(LODShapeWithShadow* shape, int id)
    {
        Reset(); _object = ::new (static_cast<void*>(_bytes)) ObjectPlain(shape, id);
        _object->SetTransform(MIdentity); return *_object;
    }
};
}
TEST_CASE("Fire query weak identity cannot inherit Ready after same-address same-ID actor replacement", "[streaming-simulation-residency]")
{
    Foundation::CaptureMainThread();
    Ref<LODShapeWithShadow> shape = new LODShapeWithShadow;
    PlacementSimulationActor storage;
    auto& first = storage.Emplace(shape, 42);
    SimulationResidencyState state;
    const SimulationResidencyState::RegionKey key{&first, uint8_t(SimulationQueryPurpose::Fire)};
    auto& record = state.regions[key]; record.actor = &first; record.actorId = first.ID();
    record.actorFrame = SimulationFrameSample(first.Transform()); record.actorRadius = first.GetRadius();
    record.status = SimulationResidencyStatus::Ready;
    const auto address = &first;
    storage.Reset();
    REQUIRE(static_cast<Object*>(record.actor) == nullptr);
    auto& replacement = storage.Emplace(shape, 42);
    REQUIRE(&replacement == address);
    REQUIRE(replacement.ID() == record.actorId);
    REQUIRE(SimulationFrameSample(replacement.Transform()) == record.actorFrame);
    REQUIRE(replacement.GetRadius() == record.actorRadius);
    REQUIRE(state.regions.find({&replacement, uint8_t(SimulationQueryPurpose::Fire)}) != state.regions.end());
    // Address/ID/pose equality cannot bypass the independent weak lifetime witness.
    REQUIRE(static_cast<Object*>(record.actor) != &replacement);
}
