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
#include <algorithm>

using namespace Poseidon;
using namespace Poseidon::Streaming;

TEST_CASE("Compact simulation metadata preflight separates complete world records from active shape work", "[streaming-simulation-residency]")
{
    const auto production = PreflightSimulationMetadata(2971218,2352,sizeof(SimulationResidencyState::Group));
    REQUIRE(production.refusal == SimulationMetadataRefusal::None);
    REQUIRE(production.recordBytes == 2352 * sizeof(SimulationResidencyState::Group));
    REQUIRE(production.recordBytes <= SimulationMaxMetadataRecordBytes);
    REQUIRE(PreflightSimulationMetadata(SimulationMaxMetadataPlacements,SimulationMaxMetadataModels,sizeof(SimulationResidencyState::Group)).refusal == SimulationMetadataRefusal::None);
    REQUIRE(PreflightSimulationMetadata(SimulationMaxMetadataPlacements+1,1,1).refusal == SimulationMetadataRefusal::PlacementCount);
    REQUIRE(PreflightSimulationMetadata(2,SimulationMaxMetadataModels+1,1).refusal == SimulationMetadataRefusal::ModelCount);
    REQUIRE(PreflightSimulationMetadata(2,1,std::numeric_limits<uint64_t>::max()).refusal == SimulationMetadataRefusal::RecordBytes);
    REQUIRE(PreflightSimulationMetadata(2,2,SimulationMaxMetadataRecordBytes).refusal == SimulationMetadataRefusal::RecordBytes);
    REQUIRE(PreflightSimulationMetadata(2,1,0).refusal == SimulationMetadataRefusal::RecordBytes);
}

TEST_CASE("Large metadata inventories never invent active shape readiness or enlarge the active model cap", "[streaming-simulation-residency]")
{
    Foundation::CaptureMainThread();
    SimulationResidencyState state;
    state.groups.resize(SimulationMaxMetadataModels);
    SECTION("unused table rows consume metadata only")
    {
        state.groups[0].memberCount = 2; state.referencedModels = 1;
        state.FinishInventory();
        REQUIRE(state.inventoryComplete);
        REQUIRE_FALSE(state.activeModelCapacityExceeded);
        REQUIRE(state.groups[0].status == SimulationResidencyState::ModelState::Waiting);
        REQUIRE(state.groups.back().status == SimulationResidencyState::ModelState::Ready);
        REQUIRE(state.stats.activeModelRefusals == 0);
    }
    SECTION("referenced active set refuses after complete scan before any shape work")
    {
        for (size_t i=0; i<SimulationMaxModels+1; ++i) ++state.groups[i].memberCount;
        state.referencedModels = SimulationMaxModels+1;
        state.FinishInventory();
        REQUIRE(state.inventoryComplete);
        REQUIRE(state.activeModelCapacityExceeded);
        REQUIRE(state.stats.activeModelRefusals == 1);
        state.FinishInventory();
        REQUIRE(state.stats.activeModelRefusals == 1);
    }
    REQUIRE_FALSE(std::any_of(state.groups.begin(),state.groups.end(),[](const auto& group) {return bool(group.shape) || group.requestOutstanding || group.charged;}));
    REQUIRE(state.leaseCounts.empty());
}

TEST_CASE("Sparse simulation lease debt supports distant placement indices without allocating world-sized rows", "[streaming-simulation-residency]")
{
    SimulationResidencyState state;
    const uint32_t distantIndex = uint32_t(SimulationMaxMetadataPlacements-1);
    REQUIRE(state.AcquireLease(distantIndex)); REQUIRE(state.AcquireLease(distantIndex));
    REQUIRE(state.leaseCounts.size() == 1);
    state.DropLease(distantIndex);
    REQUIRE(state.HasLease(distantIndex));
    state.DropLease(distantIndex);
    REQUIRE_FALSE(state.HasLease(distantIndex));
    REQUIRE(state.leaseCounts.empty());
    state.DropLease(distantIndex);
    REQUIRE(state.leaseCounts.empty());
    bool filled = true;
    for (uint32_t i=0; i<SimulationMaxLeasedPlacements; ++i) filled &= state.AcquireLease(i);
    REQUIRE(filled);
    REQUIRE_FALSE(state.AcquireLease(distantIndex));
    REQUIRE(state.AcquireLease(0)); // Existing owner's shared debt still fits.
    state.DropLease(0);
    REQUIRE_FALSE(state.AcquireLease(distantIndex)); // One owner still holds that row.
    state.DropLease(0);
    REQUIRE(state.AcquireLease(distantIndex));
    REQUIRE(state.leaseCounts.size() == SimulationMaxLeasedPlacements);
}

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
    REQUIRE(state.AcquireLease(0));
    REQUIRE(state.AcquireLease(1)); REQUIRE(state.AcquireLease(1));
    REQUIRE(state.AcquireLease(2));
    SimulationResidencyState::Region region;
    region.active = {0, 1}; region.activeSet = {0, 1};
    region.pending = {1, 2}; region.pendingSet = {1, 2}; region.pendingNew = 1;
    SECTION("cancelled incomplete replacement")
    {
        state.DropPending(region);
        REQUIRE((state.leaseCounts == std::unordered_map<uint32_t,uint32_t>{{0,1},{1,2}}));
        REQUIRE(region.active == std::vector<uint32_t>{0, 1});
        REQUIRE(region.pending.empty());
    }
    SECTION("complete replacement releases only retired old objects")
    {
        state.CompleteReplacement(region);
        REQUIRE(region.status == SimulationResidencyStatus::Ready);
        REQUIRE((state.leaseCounts == std::unordered_map<uint32_t,uint32_t>{{1,2},{2,1}}));
        REQUIRE(region.active == std::vector<uint32_t>{1, 2});
        state.DropRegion(region);
        REQUIRE((state.leaseCounts == std::unordered_map<uint32_t,uint32_t>{{1,1}}));
        // Second actor still owns placement1; releasing the first did not evict it.
        REQUIRE(region.active.empty());
    }
    SECTION("dead actor releases old plus unshared pending once")
    {
        state.DropRegion(region);
        REQUIRE((state.leaseCounts == std::unordered_map<uint32_t,uint32_t>{{1,1}}));
        state.DropRegion(region);
        REQUIRE((state.leaseCounts == std::unordered_map<uint32_t,uint32_t>{{1,1}}));
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

namespace
{
Ref<LODShapeWithShadow> LoadedPositivePlainShape()
{
    auto model = Asset::Formats::MLODLoader::load(GET_FIXTURE("mlod/p3dm_geometry_mass.p3d"));
    const auto geometry = std::find_if(model.lodLevels.begin(), model.lodLevels.end(),
        [](const auto& lod) { return lod.purpose == Model::LodPurpose::Geometry; });
    REQUIRE(geometry != model.lodLevels.end());
    auto view = *geometry, fire = *geometry;
    view.purpose = Model::LodPurpose::ViewGeometry; view.resolution = 6e15f;
    fire.purpose = Model::LodPurpose::FireGeometry; fire.resolution = 7e15f;
    model.lodLevels.push_back(std::move(view)); model.lodLevels.push_back(std::move(fire));
    ShapeBank::CpuOnlyLoadScope cpuOnly;
    Ref<LODShapeWithShadow> shape = Model::ShapeAdapter::convertToLODShape(model, false);
    REQUIRE(shape);
    REQUIRE(SimulationShapeAdmissible(*shape));
    REQUIRE(SimulationRoute(static_cast<const char*>(shape->GetPropertyClass())) == SimulationFactoryRoute::Plain);
    return shape;
}
}

TEST_CASE("Positive loaded Plain observation uses the actual instance without a model-group certificate", "[streaming-simulation-residency]")
{
    Foundation::CaptureMainThread();
    auto shape = LoadedPositivePlainShape();
    Ref<ObjectPlain> wall = new ObjectPlain(shape, 1001); wall->SetType(Primary);
    Matrix4 frame = MIdentity; frame.SetPosition(Vector3(1200, 11.5f, 1200)); wall->SetTransform(frame);
    const auto rows = SimulationFrameSample(frame);
    auto observed = SimulationResidencyState::ObservePositiveResident(*wall, 1001, rows);
    REQUIRE(observed.has_value());
    REQUIRE(observed->MatchesPositiveResident(*wall));
    SECTION("same-valued policy mutation invalidates the independent witness")
    {
        shape->SetAutoCenter(shape->IsAutoCenter());
        REQUIRE_FALSE(observed->MatchesPositiveResident(*wall));
    }
    SECTION("moved retained object is no longer an immutable authored positive")
    {
        wall->SetPosition(Vector3(100, 11.5f, 100));
        REQUIRE_FALSE(observed->MatchesPositiveResident(*wall));
        REQUIRE_FALSE(SimulationResidencyState::ObservePositiveResident(*wall, 1001, rows).has_value());
    }
    SECTION("persistent damage is excluded without dropping its save ownership")
    {
        wall->SetDammage(.25f);
        REQUIRE(wall->MustBeSaved());
        REQUIRE_FALSE(observed->MatchesPositiveResident(*wall));
        REQUIRE_FALSE(SimulationResidencyState::ObservePositiveResident(*wall, 1001, rows).has_value());
    }
    SECTION("wrong identity or factory type cannot be admitted")
    {
        REQUIRE_FALSE(SimulationResidencyState::ObservePositiveResident(*wall, 1002, rows).has_value());
        wall->SetType(Network);
        REQUIRE_FALSE(SimulationResidencyState::ObservePositiveResident(*wall, 1001, rows).has_value());
    }
}

TEST_CASE("Complete query entries share selected model ownership until joined lease cleanup", "[streaming-simulation-residency]")
{
    Foundation::CaptureMainThread();
    SimulationResidencyState state; state.groups.resize(2);
    auto shape = LoadedPositivePlainShape();
    auto& group = state.groups[0];
    group.shape = shape; group.status = SimulationResidencyState::ModelState::Ready;
    group.revision = shape->QueryPolicyRevision();
    group.charged = true; group.payloadCharge = 1234; group.shapeCharge = 5678;
    state.stats.payloadCapacityCharge = 1234; state.stats.shapeSchedulingCharge = 5678;
    Ref<ObjectPlain> first = new ObjectPlain(shape.GetRef(), 1001); first->SetType(Primary);
    Ref<ObjectPlain> second = new ObjectPlain(shape.GetRef(), 1002); second->SetType(Primary);
    SimulationResidencyState::Entry a, b;
    a.object = first.GetRef(); a.shape = shape.GetRef();
    b.object = second.GetRef(); b.shape = shape.GetRef();
    REQUIRE(state.AttachQueryBorrow(0, a)); REQUIRE(state.AttachQueryBorrow(0, b));
    REQUIRE(state.AttachQueryBorrow(0, a)); // repeated region admission owns no additional debt
    REQUIRE(group.queryEntries == 2);
    REQUIRE(group.queryBorrow.GetRef() == shape.GetRef());
    REQUIRE(state.MatchesQueryBorrow(a)); REQUIRE(state.MatchesQueryBorrow(b));
    REQUIRE_FALSE(state.AttachQueryBorrow(1, a));
    REQUIRE(group.queryEntries == 2);

    SECTION("overlapping region replacement retains the entry until the final lease retires")
    {
        state.entries.emplace(10, a); state.entries.emplace(11, b);
        SimulationResidencyState::Region old, replacement;
        REQUIRE(state.AcquireLease(10)); old.active = {10}; old.activeSet.insert(10);
        REQUIRE(state.AcquireLease(10)); replacement.active = {10}; replacement.activeSet.insert(10);
        state.DropRegion(old);
        REQUIRE(state.HasLease(10)); REQUIRE(state.MatchesQueryBorrow(state.entries.at(10)));
        state.DropRegion(replacement); REQUIRE_FALSE(state.HasLease(10));
        state.ReleaseQueryBorrow(state.entries.at(10)); state.entries.erase(10);
        REQUIRE(group.queryEntries == 1); REQUIRE(state.MatchesQueryBorrow(state.entries.at(11)));
        state.ReleaseQueryBorrow(state.entries.at(11)); state.entries.erase(11);
        REQUIRE(group.queryEntries == 0); REQUIRE_FALSE(group.queryBorrow);
    }
    SECTION("saved survivor loses query debt without losing CPU or save ownership")
    {
        second->SetDammage(.25f); REQUIRE(second->MustBeSaved());
        state.ReleaseQueryBorrow(a); REQUIRE(group.queryEntries == 1);
        state.ReleaseQueryBorrow(b); state.ReleaseQueryBorrow(b);
        REQUIRE(group.queryEntries == 0); REQUIRE_FALSE(group.queryBorrow);
        REQUIRE(second->GetShape() == shape.GetRef());
        REQUIRE(second->GetRawTotalDammage() == .25f); REQUIRE(second->MustBeSaved());
        REQUIRE(group.shape.GetRef() == shape.GetRef());
        REQUIRE(state.stats.payloadCapacityCharge == 1234);
        REQUIRE(state.stats.shapeSchedulingCharge == 5678);
    }
    SECTION("same-valued policy mutation cannot publish a current borrow")
    {
        shape->SetAutoCenter(shape->IsAutoCenter());
        REQUIRE_FALSE(state.MatchesQueryBorrow(a)); REQUIRE_FALSE(state.AttachQueryBorrow(0, a));
        SimulationResidencyState::Entry fresh; fresh.object = first.GetRef(); fresh.shape = shape.GetRef();
        REQUIRE_FALSE(state.AttachQueryBorrow(0, fresh)); REQUIRE(group.queryEntries == 2);
        state.ReleaseQueryBorrow(a); state.ReleaseQueryBorrow(b);
        REQUIRE_FALSE(group.queryBorrow);
    }
    SECTION("another actual shape cannot borrow the registered producer")
    {
        auto alias = LoadedPositivePlainShape();
        Ref<ObjectPlain> other = new ObjectPlain(alias.GetRef(), 1003); other->SetType(Primary);
        SimulationResidencyState::Entry wrong; wrong.object = other.GetRef(); wrong.shape = alias.GetRef();
        REQUIRE_FALSE(state.AttachQueryBorrow(0, wrong)); REQUIRE(group.queryEntries == 2);
    }
    SECTION("incomplete large-inventory coverage cannot acquire a selected query borrow")
    {
        state.activeModelCapacityExceeded = true;
        SimulationResidencyState::Entry fresh; fresh.object = first.GetRef(); fresh.shape = shape.GetRef();
        REQUIRE_FALSE(state.AttachQueryBorrow(0, fresh));
        REQUIRE(group.queryEntries == 2);
        state.ReleaseQueryBorrow(a); state.ReleaseQueryBorrow(b);
        REQUIRE_FALSE(group.queryBorrow);
    }
}

TEST_CASE("Actor positive replacement retains loaded geometry on large inventories without making Fire ready", "[streaming-simulation-residency]")
{
    Foundation::CaptureMainThread();
    SimulationResidencyState state;
    state.groups.resize(SimulationMaxModels + 1);
    for (auto& group : state.groups) group.memberCount = 1;
    state.referencedModels = state.groups.size(); state.FinishInventory();
    REQUIRE(state.activeModelCapacityExceeded);
    SECTION("complete query watch refusal does not prevent independent positive observation")
    {
        state.watchCapacityExceeded = true;
    }
    auto shape = LoadedPositivePlainShape();
    Ref<ObjectPlain> wall = new ObjectPlain(shape, 1001); wall->SetType(Primary);
    Matrix4 frame = MIdentity; frame.SetPosition(Vector3(1200, 11.5f, 1200)); wall->SetTransform(frame);
    auto observed = SimulationResidencyState::ObservePositiveResident(*wall, 1001, SimulationFrameSample(frame));
    REQUIRE(observed.has_value());
    state.entries[0] = std::move(*observed);
    auto& actor = state.regions[{wall.GetRef(), 0}]; actor.actor = wall.GetRef();
    actor.pending = {0}; actor.pendingSet = {0}; actor.pendingNew = 1;
    REQUIRE(state.AcquireLease(0));
    state.CompleteReplacement(actor, false);
    REQUIRE(actor.positiveOnly); REQUIRE(actor.positiveScanComplete);
    REQUIRE(actor.status == SimulationResidencyStatus::Pending);
    REQUIRE(state.HasLease(0));
    REQUIRE(state.entries[0].MatchesPositiveResident(*wall));
    REQUIRE_FALSE(std::any_of(state.groups.begin(), state.groups.end(), [](const auto& g)
        { return bool(g.shape) || g.requestOutstanding || g.charged; }));
    auto& fire = state.regions[{wall.GetRef(), uint8_t(SimulationQueryPurpose::Fire)}];
    fire.actor = wall.GetRef();
    REQUIRE(fire.status == SimulationResidencyStatus::Pending);
    REQUIRE(fire.active.empty());
    REQUIRE(state.activeModelCapacityExceeded);
    SECTION("moving actor completes an empty new selection and releases old lease")
    {
        state.DropPending(actor); // New descriptor: preserve old active until replacement completes.
        REQUIRE_FALSE(actor.positiveScanComplete); REQUIRE(state.HasLease(0));
        state.CompleteReplacement(actor, false);
        REQUIRE_FALSE(state.HasLease(0)); REQUIRE(actor.active.empty());
        REQUIRE(actor.status != SimulationResidencyStatus::Ready);
    }
    SECTION("actor expiry balances pending and active positive debt")
    {
        state.DropRegion(actor);
        REQUIRE_FALSE(state.HasLease(0)); REQUIRE(actor.active.empty());
    }
    REQUIRE(state.activeModelCapacityExceeded);
    REQUIRE(fire.status != SimulationResidencyStatus::Ready);
}

TEST_CASE("Late camera admissions cannot repeatedly roll back an unfinished positive lease prefix", "[streaming-simulation-residency]")
{
    SimulationResidencyState state;
    auto& region = state.regions[{nullptr, 0}];
    region.positiveOnly = true; region.planGeneration = state.generation;
    region.positiveAdmissionGeneration = state.positiveAdmissionGeneration;
    region.cell = 7; region.offset = 12;
    REQUIRE(state.AcquireLease(10));
    region.pending = {10}; region.pendingSet = {10}; region.pendingNew = 1;
    // Camera populates already-visited cells and later cells while this scan
    // progresses. Preserve the cursor and debt instead of restarting its prefix.
    for (int admission = 0; admission < 100; ++admission)
    {
        ++state.positiveAdmissionGeneration;
        REQUIRE_FALSE(region.NeedsPositiveRestart(state.generation, state.positiveAdmissionGeneration));
    }
    REQUIRE(region.cell == 7); REQUIRE(region.offset == 12);
    REQUIRE(state.HasLease(10));
    REQUIRE(state.AcquireLease(20));
    region.pending.push_back(20); region.pendingSet.insert(20); ++region.pendingNew;
    state.CompleteReplacement(region, false);
    REQUIRE(region.NeedsPositiveRestart(state.generation, state.positiveAdmissionGeneration));
    REQUIRE(region.status != SimulationResidencyStatus::Ready);
    REQUIRE(region.active == std::vector<uint32_t>{10, 20});
    // Next pump revisits stale completed selection, retaining active geometry
    // until the new complete selection includes a late-admitted earlier object.
    state.DropPending(region);
    region.positiveAdmissionGeneration = state.positiveAdmissionGeneration;
    region.planGeneration = state.generation;
    REQUIRE_FALSE(region.NeedsPositiveRestart(state.generation, state.positiveAdmissionGeneration));
    REQUIRE(state.HasLease(10)); REQUIRE(state.HasLease(20));
    REQUIRE(state.AcquireLease(30));
    region.pending = {20, 30}; region.pendingSet = {20, 30}; region.pendingNew = 1;
    state.CompleteReplacement(region, false);
    REQUIRE_FALSE(state.HasLease(10)); REQUIRE(state.HasLease(20)); REQUIRE(state.HasLease(30));
    REQUIRE_FALSE(region.NeedsPositiveRestart(state.generation, state.positiveAdmissionGeneration));
    REQUIRE(region.NeedsPositiveRestart(state.generation + 1, state.positiveAdmissionGeneration));
    REQUIRE(region.status != SimulationResidencyStatus::Ready);
    state.DropRegion(region);
    REQUIRE(state.leaseCounts.empty());
}

TEST_CASE("Cold positive source bounds reject a subsequently transformed cached shape", "[streaming-simulation-residency]")
{
    Foundation::CaptureMainThread();
    auto shape = LoadedPositivePlainShape();
    // Original synthetic audited static IR used only to exercise the owner
    // bound predicate; parser provenance is covered by source-audit tests.
    auto source = Asset::Formats::MLODLoader::load(GET_FIXTURE("mlod/p3dm_geometry_mass.p3d"));
    const auto geometry = std::find_if(source.lodLevels.begin(), source.lodLevels.end(),
        [](const auto& lod) { return lod.purpose == Model::LodPurpose::Geometry; });
    REQUIRE(geometry != source.lodLevels.end());
    auto view = *geometry, fire = *geometry;
    view.purpose = Model::LodPurpose::ViewGeometry; view.resolution = 6e15f;
    fire.purpose = Model::LodPurpose::FireGeometry; fire.resolution = 7e15f;
    source.lodLevels.push_back(view); source.lodLevels.push_back(fire);
    source.sourceFormat = "ODOL"; source.sourceVersion = 7; source.allowAnimation = false;
    source.boundingSphere.radius = shape->BoundingSphere();
    for (auto& lod : source.lodLevels)
    {
        for (auto& vertex : lod.mesh.vertices) vertex.flags = Model::VertexFlags{};
        for (auto& selection : lod.mesh.selections)
        { selection.vertexWeights.clear(); selection.sourceVertexWeights.clear(); }
    }
    auto& audit = source.sourceAudit;
    audit.producerVersion = 1; audit.sourceRevision = 7;
    audit.geometryCoverage = Model::SourceGeometryCoverage::DeclaredLodsDecoded;
    audit.observations = Model::SourceAuditAllObservations;
    audit.declaredLods = audit.decodedLods = uint32_t(source.lodLevels.size());
    const auto envelope = BuildStaticSourceEnvelope(source, 42, StaticSourceCoverage::FullCompiledIR);
    REQUIRE(envelope.State() == StaticSourceEnvelopeState::SourceEvidence);
    REQUIRE(PositiveColdShapeMatchesSource(*shape, envelope, 42));
    REQUIRE_FALSE(PositiveColdShapeMatchesSource(*shape, envelope, 43));
    shape->InternalTransform(Matrix4(MScale, 1000));
    REQUIRE(shape->BoundingSphere() > *envelope.RadiusForGeneration(42));
    REQUIRE_FALSE(PositiveColdShapeMatchesSource(*shape, envelope, 42));
}

TEST_CASE("Cold selection cannot replace legacy ownership before a shape exists", "[streaming-simulation-residency]")
{
    SimulationResidencyState::Group group;
    REQUIRE(group.CanEnlistPositiveCold());
    SECTION("pending legacy request without a published shape") { group.requestOutstanding = true; }
    SECTION("interrupted charged handoff without a published shape")
    { group.charged = true; group.payloadCharge = 4096; group.shapeCharge = 1048576; }
    SECTION("residual accounting cannot be silently overwritten") { group.payloadCharge = 4096; }
    SECTION("previously validated legacy source") { group.sourceValidated = true; }
    SECTION("legacy refusal remains a refusal") { group.status = SimulationResidencyState::ModelState::Unknown; }
    REQUIRE_FALSE(group.shape);
    REQUIRE_FALSE(group.CanEnlistPositiveCold());
    REQUIRE_FALSE(group.positiveCold);
}

TEST_CASE("Selected cold retirement balances bounded facts and payload charges without certifying groups", "[streaming-simulation-residency]")
{
    Foundation::CaptureMainThread();
    SimulationResidencyState state; state.groups.resize(SimulationMaxModels + 1);
    for (auto& group : state.groups) group.memberCount = 1;
    state.referencedModels = state.groups.size(); state.FinishInventory();
    REQUIRE(state.activeModelCapacityExceeded);
    auto& selected = state.groups[0];
    selected.positiveCold = std::make_unique<SimulationResidencyState::PositiveColdFacts>();
    selected.positiveCold->factCharge = 512;
    selected.positiveCold->stage = SimulationResidencyState::PositiveColdStage::Held;
    selected.shape = LoadedPositivePlainShape();
    selected.charged = true; selected.payloadCharge = 4096; selected.shapeCharge = 1048576;
    state.positiveColdWork.push_back(0);
    state.stats.positiveColdFactsBytes = 512;
    state.stats.payloadCapacityCharge = 4096; state.stats.shapeSchedulingCharge = 1048576;
    REQUIRE(selected.status == SimulationResidencyState::ModelState::Waiting);
    SECTION("expired selection releases held working geometry") { state.RetirePositiveCold(0); }
    SECTION("rejected final owner tail prevents retries in this inventory")
    { state.RetirePositiveCold(0, true); REQUIRE(selected.positiveColdRefused); }
    REQUIRE_FALSE(selected.positiveCold); REQUIRE_FALSE(selected.shape);
    REQUIRE(state.positiveColdWork.empty());
    REQUIRE(state.positiveColdWork.capacity() == 0);
    REQUIRE(state.stats.positiveColdFactsBytes == 0);
    REQUIRE(state.stats.payloadCapacityCharge == 0); REQUIRE(state.stats.shapeSchedulingCharge == 0);
    REQUIRE(selected.status == SimulationResidencyState::ModelState::Waiting);
    REQUIRE(state.activeModelCapacityExceeded);
    state.RetirePositiveCold(0); // Joined cleanup can observe an already retired job.
    REQUIRE(state.stats.positiveColdFactsBytes == 0);
}

TEST_CASE("Identical query centre lines contain nested engine float radii without clearance erosion", "[streaming-simulation-residency]")
{
    const CoveragePoint from{1190,11.5,1200}, to{1210,11.5,1200};
    const double actualFloatRadius = double(.01f);
    REQUIRE(actualFloatRadius < .01);
    REQUIRE(SimulationCapsuleContains(from, to, .01, from, to, actualFloatRadius));
    REQUIRE(SimulationCapsuleContains(from, to, .01, to, from, actualFloatRadius));
    REQUIRE(SimulationCapsuleContains(from, to, .01, from, to, 0));
    REQUIRE(SimulationCapsuleContains(from, from, .01, from, from, actualFloatRadius));
    REQUIRE(SimulationCapsuleContains(from, from, .01, from, from, 0));
    REQUIRE(SimulationCapsuleContains(from, from, 0, from, from, 0));
    REQUIRE_FALSE(SimulationCapsuleContains(from, to, .01, from, to, .02));
    REQUIRE_FALSE(SimulationCapsuleContains(from, from, .01, from, from, .02));
    // Upward float rounding cannot invent extra source radius. Callers should
    // canonicalise queue/status and geometry radius to that same stored float.
    REQUIRE_FALSE(SimulationCapsuleContains(from, to, .1, from, to, double(.1f)));
    REQUIRE(SimulationCapsuleContains(from, to, double(.1f), from, to, double(.1f)));
    auto displaced = to;
    displaced[2] += .00001;
    REQUIRE_FALSE(SimulationCapsuleContains(from, to, .01, from, displaced, actualFloatRadius));
}

TEST_CASE("Fire query channel cannot override automatic neighborhood or inherit its Ready state", "[streaming-simulation-residency]")
{
    Foundation::CaptureMainThread();
    Ref<LODShapeWithShadow> shape = new LODShapeWithShadow;
    Ref<ObjectPlain> actor = new ObjectPlain(shape, 42);
    SimulationResidencyState state;
    REQUIRE(state.AcquireLease(0)); REQUIRE(state.AcquireLease(1));
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
    REQUIRE((state.leaseCounts == std::unordered_map<uint32_t,uint32_t>{{0,1}}));
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
