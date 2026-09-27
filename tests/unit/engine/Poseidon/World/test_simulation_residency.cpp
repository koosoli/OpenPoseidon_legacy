#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/World/Terrain/SimulationResidencyState.hpp>
#include <Poseidon/World/Terrain/SimulationResidencyPolicy.hpp>
#include <Poseidon/World/Terrain/SimulationFireCollision.hpp>
#include <Poseidon/Asset/Formats/P3D/MLODLoader.hpp>
#include <Poseidon/World/Model/ShapeAdapter.hpp>
#include <Poseidon/IO/ParamFile/InitLibraryElement.hpp>
#include <Poseidon/IO/Streams/QBStream.hpp>
#include "test_fixtures.hpp"
#include <limits>
#include <cstddef>
#include <new>
#include <algorithm>

using namespace Poseidon;
using namespace Poseidon::Streaming;

TEST_CASE("Fire collision consumption is shared until the joined owner boundary", "[streaming-simulation-residency]")
{
    SimulationFireCollisionConsumption budget;
    using Clock = std::chrono::steady_clock;
    auto now = Clock::time_point{};
    const auto clock = [&]() noexcept { return now; };
    SECTION("NonReady and exception exits charge the same actual work")
    {
        REQUIRE(budget.TryBegin());
        {
            SimulationFireCollisionCharge charge(budget, clock);
            now += std::chrono::microseconds(750); // An Invalid/Pending return still leaves this lexical scope.
        }
        REQUIRE(budget.ChargedMilliseconds() == .75);
        REQUIRE(budget.TryBegin());
        try
        {
            SimulationFireCollisionCharge charge(budget, clock);
            now += std::chrono::microseconds(1250);
            throw 1;
        }
        catch (int) {}
        REQUIRE(budget.ChargedMilliseconds() == 2);
        REQUIRE_FALSE(budget.TryBegin());
        REQUIRE(budget.Attempts() == 2);
        budget.Reset(); // Same reset used before Pump's capacity-refusal return.
        REQUIRE(budget.ChargedMilliseconds() == 0);
        REQUIRE(budget.Attempts() == 0);
        REQUIRE(budget.TryBegin());
    }
    SECTION("Cheap refusals cannot bypass the attempt cap")
    {
        for (size_t i = 0; i < SimulationFireCollisionConsumption::MaxAttempts; ++i)
        {
            REQUIRE(budget.TryBegin());
            SimulationFireCollisionCharge charge(budget, clock);
        }
        REQUIRE_FALSE(budget.TryBegin());
        REQUIRE(budget.Attempts() == SimulationFireCollisionConsumption::MaxAttempts);
        budget.Reset();
        REQUIRE(budget.TryBegin());
    }
    SECTION("Indivisible overshoot and invalid clock observations fail closed")
    {
        REQUIRE(budget.TryBegin());
        budget.Charge(25); // No hard-duration claim for one indivisible operation.
        REQUIRE_FALSE(budget.TryBegin());
        REQUIRE(budget.ChargedMilliseconds() == 2);
        for (const double invalid : {-1.0, std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()})
        {
            budget.Reset();
            REQUIRE(budget.TryBegin());
            budget.Charge(invalid);
            REQUIRE_FALSE(budget.TryBegin());
        }
    }
}

TEST_CASE("Fire interest requires owner heartbeats and expires independently of automatic visits", "[streaming-simulation-residency]")
{
    using Clock = SimulationFireHeartbeat::Clock;
    const auto start = Clock::time_point{} + std::chrono::seconds(10);
    SimulationResidencyState::Region region;
    const auto fire = uint8_t(SimulationQueryPurpose::Fire);
    REQUIRE(region.FireInterestExpired(fire, start));
    REQUIRE_FALSE(region.FireInterestExpired(0, start));
    region.fireHeartbeat.Renew(start);
    REQUIRE_FALSE(region.FireInterestExpired(fire, start + std::chrono::milliseconds(1999)));
    REQUIRE(region.FireInterestExpired(fire, start + SimulationFireHeartbeat::Grace));
    // Initial validation was fresh, but expensive owner work finished at the
    // deadline: status and snapshot publication use this same final gate.
    REQUIRE(region.FireStatusAtPublication(fire, SimulationResidencyStatus::Ready,
        start + std::chrono::milliseconds(1999)) == SimulationResidencyStatus::Ready);
    REQUIRE(region.FireStatusAtPublication(fire, SimulationResidencyStatus::Ready,
        start + SimulationFireHeartbeat::Grace) == SimulationResidencyStatus::Pending);
    REQUIRE(region.FireStatusAtPublication(0, SimulationResidencyStatus::Ready,
        start + SimulationFireHeartbeat::Grace) == SimulationResidencyStatus::Ready);
    REQUIRE(region.FireStatusAtPublication(fire, SimulationResidencyStatus::Unknown,
        start + SimulationFireHeartbeat::Grace) == SimulationResidencyStatus::Unknown);
    // Read-only polls and automatic clocks do not change the Fire heartbeat.
    for (size_t poll = 0; poll < 16; ++poll)
    {
        ++region.seen; ++region.automaticSeenVisit;
        REQUIRE_FALSE(region.FireInterestExpired(fire, start + std::chrono::seconds(1)));
    }
    REQUIRE(region.FireInterestExpired(fire, start + std::chrono::seconds(3)));
    region.fireHeartbeat.Renew(start + std::chrono::seconds(3));
    REQUIRE_FALSE(region.FireInterestExpired(fire, start + std::chrono::seconds(4)));
    REQUIRE(region.FireInterestExpired(fire, start + std::chrono::seconds(5)));
}

TEST_CASE("Expired Fire contact publication refuses even when bounded retirement leaves records alive", "[streaming-simulation-residency]")
{
    Foundation::CaptureMainThread();
    using Clock = SimulationFireHeartbeat::Clock;
    const auto start = Clock::time_point{} + std::chrono::seconds(10);
    const auto expired = start + SimulationFireHeartbeat::Grace;
    const auto fire = uint8_t(SimulationQueryPurpose::Fire);
    Ref<LODShapeWithShadow> shape = new LODShapeWithShadow;
    Ref<ObjectPlain> actor = new ObjectPlain(shape, 42);
    SimulationResidencyState state;
    SimulationResidencyState::Region automatic;
    automatic.actor = actor.GetRef(); automatic.automatic = true;
    automatic.active = {0}; automatic.activeSet = {0}; REQUIRE(state.AcquireLease(0));
    std::array<SimulationResidencyState::Region, 5> queries;
    for (auto& region : queries)
    {
        region.actor = actor.GetRef(); region.status = SimulationResidencyStatus::Ready;
        region.fireHeartbeat.Renew(start);
        region.active = {0}; region.activeSet = {0}; REQUIRE(state.AcquireLease(0));
        region.pending = {0, 1}; region.pendingSet = {0, 1}; REQUIRE(state.AcquireLease(1));
    }
    // Existing owner cleanup can retire four, but every remaining record is
    // already unusable for admission/status/contact publication at its deadline.
    for (size_t i = 0; i < 4; ++i) state.DropRegion(queries[i]);
    REQUIRE(state.leaseCounts.at(0) == 2); REQUIRE(state.leaseCounts.at(1) == 1);
    auto& held = queries.back();
    REQUIRE(held.status == SimulationResidencyStatus::Ready);
    REQUIRE(held.FireInterestExpired(fire, expired));
    CollisionBuffer contacts; CollisionInfo sentinel{}; sentinel.object = actor.GetRef(); sentinel.component = 99;
    contacts.Add(sentinel);
    bool legacyCalled = false, directCalled = false;
    Object* candidates[] = {actor.GetRef()};
    const auto status = AppendSimulationFireContacts(contacts, std::span<Object* const>(candidates, 1), nullptr, nullptr,
        [&](CollisionBuffer&) { legacyCalled = true; },
        [&](CollisionBuffer&, Object&) { directCalled = true; },
        [&] { return held.FireInterestExpired(fire, expired) ? SimulationResidencyStatus::Pending : held.status; },
        [] { return true; });
    REQUIRE(status == SimulationResidencyStatus::Pending);
    REQUIRE_FALSE(legacyCalled); REQUIRE_FALSE(directCalled);
    REQUIRE(contacts.Size() == 1); REQUIRE(contacts[0].component == 99); REQUIRE(contacts[0].object.GetRef() == actor.GetRef());
    // A fresh query can also expire during an indivisible legacy trace. Its
    // staged hit must not escape the final production publication validation.
    size_t validations = 0;
    const auto crossed = AppendSimulationFireContacts(contacts, std::span<Object* const>(candidates, 1), nullptr, nullptr,
        [&](CollisionBuffer& staged) { auto hit = sentinel; hit.component = 100; staged.Add(hit); },
        [&](CollisionBuffer&, Object&) { FAIL("Legacy contact already covers this candidate"); },
        [&] {
            const auto now = validations++ == 0 ? start : expired;
            return held.FireStatusAtPublication(fire, held.status, now);
        }, [] { return true; });
    REQUIRE(crossed == SimulationResidencyStatus::Pending); REQUIRE(validations == 2);
    REQUIRE(contacts.Size() == 1); REQUIRE(contacts[0].component == 99);
    state.DropRegion(held);
    REQUIRE((state.leaseCounts == std::unordered_map<uint32_t,uint32_t>{{0,1}}));
    REQUIRE_FALSE(automatic.FireInterestExpired(0, expired));
    state.DropRegion(automatic); REQUIRE(state.leaseCounts.empty());
}

TEST_CASE("Automatic actor expiry follows actual sparse visitation rather than pump count", "[streaming-simulation-residency]")
{
    SimulationResidencyState state;
    SimulationResidencyState::Region region;
    region.automatic = true;
    state.automaticActorVisits = AdvanceSimulationActorVisit(state.automaticActorVisits);
    region.automaticSeenVisit = state.automaticActorVisits;
    // One actual slot per pump in a 64-slot registry: the old 12-pump
    // allowance expired before the actor's next legitimate visit.
    for (size_t pump = 0; pump < 256; ++pump)
    {
        ++state.tick;
        state.automaticActorVisits = AdvanceSimulationActorVisit(state.automaticActorVisits);
        REQUIRE_FALSE(SimulationAutomaticRegionExpired(true, true, 64,
            state.automaticActorVisits, region.automaticSeenVisit));
        if ((pump + 1) % 64 == 0) region.automaticSeenVisit = state.automaticActorVisits;
    }
    const auto lastVisit = region.automaticSeenVisit;
    for (size_t pump = 0; pump < 1024; ++pump) ++state.tick; // Budget permits no slot work.
    REQUIRE(state.automaticActorVisits == lastVisit);
    REQUIRE_FALSE(SimulationAutomaticRegionExpired(true, true, 64, state.automaticActorVisits, lastVisit));
    // At full progress retain the old grace: 12 nominal batches of 32.
    REQUIRE_FALSE(SimulationAutomaticRegionExpired(true, true, 64, lastVisit + 384, lastVisit));
    REQUIRE(SimulationAutomaticRegionExpired(true, true, 64, lastVisit + 385, lastVisit));
    REQUIRE_FALSE(SimulationAutomaticRegionExpired(true, true, 1, 10, 0));
    REQUIRE(SimulationAutomaticRegionExpired(true, true, 1, 11, 0));
    REQUIRE_FALSE(SimulationAutomaticRegionExpired(false, true, 64, lastVisit + 1000, lastVisit));
    const auto maximum = std::numeric_limits<uint64_t>::max();
    REQUIRE(AdvanceSimulationActorVisit(maximum) == maximum);
    REQUIRE_FALSE(SimulationAutomaticRegionExpired(true, true, 64, maximum, 0));
    REQUIRE_FALSE(SimulationAutomaticRegionExpired(true, true, 64, 0, 1));
}

TEST_CASE("Empty actor registry retires only automatic leases and deleted weak actors release all purposes", "[streaming-simulation-residency]")
{
    Foundation::CaptureMainThread();
    Ref<LODShapeWithShadow> shape = new LODShapeWithShadow;
    Ref<ObjectPlain> actor = new ObjectPlain(shape, 42);
    SimulationResidencyState state;
    SimulationResidencyState::Region automatic, manual;
    automatic.actor = actor.GetRef(); automatic.automatic = true;
    manual.actor = actor.GetRef(); manual.automatic = false;
    REQUIRE(state.AcquireLease(0)); REQUIRE(state.AcquireLease(0)); REQUIRE(state.AcquireLease(1));
    automatic.active = {0}; automatic.activeSet = {0};
    automatic.pending = {0, 1}; automatic.pendingSet = {0, 1}; automatic.pendingNew = 1;
    manual.active = {0}; manual.activeSet = {0};
    REQUIRE(SimulationAutomaticRegionExpired(automatic.automatic, static_cast<Object*>(automatic.actor) != nullptr,
        0, state.automaticActorVisits, automatic.automaticSeenVisit));
    REQUIRE_FALSE(SimulationAutomaticRegionExpired(manual.automatic, static_cast<Object*>(manual.actor) != nullptr,
        0, state.automaticActorVisits, manual.automaticSeenVisit));
    state.DropRegion(automatic);
    REQUIRE((state.leaseCounts == std::unordered_map<uint32_t,uint32_t>{{0,1}}));
    REQUIRE(automatic.active.empty()); REQUIRE(automatic.pending.empty());
    actor = nullptr;
    REQUIRE(static_cast<Object*>(manual.actor) == nullptr);
    REQUIRE(SimulationAutomaticRegionExpired(false, false, 64, 0, 0));
    state.DropRegion(manual);
    REQUIRE(state.leaseCounts.empty());
    state.DropRegion(automatic); state.DropRegion(manual);
    REQUIRE(state.leaseCounts.empty());
}

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

namespace
{
void ParseRegisteredConfig(ParamFile& root, const std::string& text)
{
    InitLibraryElement(); QIStream input(text.data(), static_cast<int>(text.size())); root.Parse(input);
}
struct RegisteredPlainInventory
{
    ShapeBank bank;
    ParamFile config;
    SimulationResidencyState state;
    std::vector<RStringB> names;
    std::shared_ptr<Model::Model> prototype;
    // External owners represent the normal camera/object ownership; the bank
    // and metadata hold weak witnesses only. No simulation-owned global array.
    std::vector<Ref<LODShapeWithShadow>> external;
    explicit RegisteredPlainInventory(size_t count)
    {
        Foundation::CaptureMainThread();
        std::string configText = "class CfgModels {";
        for (size_t i = 0; i < count; ++i)
            configText += "class registered_" + std::to_string(i) + " { properties[]={\"class\",\"\"}; };";
        for (size_t i = 0; i < 128; ++i)
            configText += "class unused_" + std::to_string(i) + " { properties[]={\"class\",\"house\"}; };";
        configText += "};"; ParseRegisteredConfig(config, configText);
        auto model = Asset::Formats::MLODLoader::load(GET_FIXTURE("mlod/p3dm_geometry_mass.p3d"));
        const auto geometry = std::find_if(model.lodLevels.begin(), model.lodLevels.end(),
            [](const auto& lod) { return lod.purpose == Model::LodPurpose::Geometry; });
        REQUIRE(geometry != model.lodLevels.end());
        auto view = *geometry, fire = *geometry;
        view.purpose = Model::LodPurpose::ViewGeometry; view.resolution = 6e15f;
        fire.purpose = Model::LodPurpose::FireGeometry; fire.resolution = 7e15f;
        model.lodLevels.push_back(std::move(view)); model.lodLevels.push_back(std::move(fire));
        prototype = std::make_shared<Model::Model>(model);
        state.groups.resize(count); names.reserve(count); external.reserve(count);
        ShapeBank::CpuOnlyLoadScope cpuOnly;
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto name = "registered_" + std::to_string(i) + ".p3d";
            names.emplace_back(name.c_str()); model.sourcePath = name;
            external.emplace_back(bank.NewFromModel(name.c_str(), false, true, std::make_shared<Model::Model>(model), nullptr));
            REQUIRE(external.back());
            auto& group = state.groups[i]; group.memberCount = 1; group.maximumScale = 1;
            group.origins = {{double(i ? 650 : 1200), 10, double(i ? 650 : 1200)},
                             {double(i ? 650 : 1230), 10, double(i ? 650 : 1230)}};
        }
        state.referencedModels = count; state.FinishInventory();
        for (uint32_t i = 0; i < count; ++i)
        {
            bool changed = false;
            REQUIRE(state.RefreshRegisteredModel(i, bank, names[i], config, changed, [] { return true; }) == SimulationResidencyStatus::Ready);
            REQUIRE(changed);
        }
    }
};
}

TEST_CASE("Admitted small inventory rechecks actual off-bin geometry before fire can publish contacts", "[streaming-simulation-residency][streaming-simulation-admitted]")
{
    RegisteredPlainInventory fixture(2); auto& state = fixture.state;
    REQUIRE_FALSE(state.activeModelCapacityExceeded);
    for (size_t i = 0; i < state.groups.size(); ++i)
    {
        auto& group = state.groups[i]; group.shape = fixture.external[i];
        group.radius = group.shape->BoundingSphere(); group.revision = group.shape->QueryPolicyRevision();
        group.route = SimulationFactoryRoute::Plain; group.sourceValidated = true;
        group.status = SimulationResidencyState::ModelState::Ready;
    }
    REQUIRE(state.ValidateAdmittedMetadata([] { return true; }) == SimulationResidencyStatus::Ready);
    auto* changed = fixture.external.back().GetRef(); const auto revision = changed->QueryPolicyRevision();
    auto* geometry = changed->GeometryLevel(); REQUIRE(geometry); REQUIRE(geometry->NPos() > 0);
    SECTION("unversioned off-bin point escapes parent bounds") { geometry->SetPos(0) = Vector3(1000000, 0, 0); }
    SECTION("unversioned authored LOD sphere is too small")
    { geometry->SetMinMax(geometry->Min(), geometry->Max(), geometry->BSphereCenter(), 0); }
    SECTION("unversioned LOD box no longer encloses its source points")
    { geometry->MinMax()[0] = Vector3(1000000, 1000000, 1000000); }
    SECTION("unversioned conform flag invalidates the static query contract")
    { geometry->SetClip(0, geometry->Clip(0) | ClipLandKeep); }
    REQUIRE(changed->QueryPolicyRevision() == revision);
    REQUIRE(state.groups.back().origins.min[0] == 650); // Outside the selected wall's origin cells.
    REQUIRE(state.ValidateAdmittedMetadata([] { return true; }) == SimulationResidencyStatus::Unknown);
    Ref<ObjectPlain> wall = new ObjectPlain(fixture.external.front(), 1001); wall->SetType(Primary);
    CollisionBuffer contacts; CollisionInfo sentinel{}; sentinel.object = wall.GetRef(); sentinel.component = 99; contacts.Add(sentinel);
    const std::array<Object*, 1> candidates{wall.GetRef()};
    bool traced = false;
    const auto status = AppendSimulationFireContacts(contacts, std::span<Object* const>(candidates.data(), candidates.size()), nullptr, nullptr,
        [&](CollisionBuffer&) { traced = true; }, [&](CollisionBuffer&, Object&) { traced = true; },
        [&] { return state.ValidateAdmittedMetadata([] { return true; }); }, [] { return true; });
    REQUIRE(status == SimulationResidencyStatus::Unknown); REQUIRE_FALSE(traced);
    REQUIRE(contacts.Size() == 1); REQUIRE(contacts[0].object.GetRef() == wall.GetRef()); REQUIRE(contacts[0].component == 99);
}

TEST_CASE("Small admitted metadata shares one deadline across all referenced source shapes", "[streaming-simulation-residency][streaming-simulation-admitted]")
{
    RegisteredPlainInventory fixture(2); auto& state = fixture.state;
    for (size_t i = 0; i < state.groups.size(); ++i)
    {
        auto& group = state.groups[i]; group.shape = fixture.external[i];
        group.radius = group.shape->BoundingSphere(); group.revision = group.shape->QueryPolicyRevision();
        group.route = SimulationFactoryRoute::Plain; group.status = SimulationResidencyState::ModelState::Ready;
    }
    size_t oneModelVisits = 0;
    REQUIRE(SimulationResidencyState::AuditRegisteredGeometryNow(*fixture.external.front(), [&] { ++oneModelVisits; return true; }) == SimulationResidencyStatus::Ready);
    size_t visits = 0;
    // Allows one complete model plus its group check, then expires at the next
    // model. A per-model budget reset would incorrectly report this world Ready.
    REQUIRE(state.ValidateAdmittedMetadata([&] { return ++visits <= oneModelVisits + 1; }) == SimulationResidencyStatus::Pending);
    REQUIRE(state.ValidateAdmittedMetadata([] { return true; }) == SimulationResidencyStatus::Ready);
}

TEST_CASE("Audited finite ODOL source cannot certify an undersized authored off-bin parent sphere", "[streaming-simulation-residency][streaming-simulation-admitted]")
{
    RegisteredPlainInventory fixture(1);
    auto source = std::make_shared<Model::Model>(*fixture.prototype);
    source->sourceFormat = "ODOL"; source->sourceVersion = 7; source->sourcePath = "undersized_offbin.p3d";
    source->boundingSphere.radius = .001f;
    source->sourceAudit.producerVersion = 1; source->sourceAudit.sourceRevision = 7;
    source->sourceAudit.geometryCoverage = Model::SourceGeometryCoverage::DeclaredLodsDecoded;
    source->sourceAudit.observations = Model::SourceAuditAllObservations;
    source->sourceAudit.declaredLods = source->sourceAudit.decodedLods = uint32_t(source->lodLevels.size());
    REQUIRE(SimulationSourceAdmissible(*source));
    ShapeBank::CpuOnlyLoadScope cpuOnly;
    Ref<LODShapeWithShadow> shape = fixture.bank.NewFromModel("undersized_offbin.p3d", false, true, source, nullptr);
    REQUIRE(shape); REQUIRE(shape->BoundingSphere() == .001f);
    REQUIRE(SimulationShapeAdmissible(*shape)); // Bounds, not missing roles, must refuse.
    bool outsideParent = false;
    auto* actual = shape->GeometryLevel(); REQUIRE(actual);
    for (int i = 0; i < actual->NPos(); ++i)
        outsideParent |= actual->Pos(i).SquareSize() > shape->BoundingSphere() * shape->BoundingSphere();
    REQUIRE(outsideParent); // Actual adapter output, not an inferred source-space bound.
    auto& state = fixture.state; auto& group = state.groups.front();
    group.shape = shape; group.radius = shape->BoundingSphere(); group.revision = shape->QueryPolicyRevision();
    group.route = SimulationFactoryRoute::Plain; group.sourceValidated = true; group.status = SimulationResidencyState::ModelState::Ready;
    group.origins = {{650, 10, 650}, {650, 10, 650}};
    REQUIRE(state.ValidateAdmittedMetadata([] { return true; }) == SimulationResidencyStatus::Unknown);
}

TEST_CASE("Complete registered bank metadata can select one strong model from 257 without a full source producer", "[streaming-simulation-residency]")
{
    RegisteredPlainInventory fixture(SimulationMaxModels + 1); auto& s = fixture.state;
    REQUIRE(s.activeModelCapacityExceeded);
    REQUIRE(s.AdvanceRegisteredPadding(fixture.bank, fixture.names, fixture.config, [] { return true; }) == SimulationResidencyStatus::Ready);
    REQUIRE(s.registeredMetadataEverComplete); REQUIRE_FALSE(s.registeredMetadataDirty);
    REQUIRE(s.placementPadding >= fixture.external[0]->BoundingSphere());
    REQUIRE(s.registeredBorrowedModels == 0);
    for (const auto& g : s.groups) { REQUIRE_FALSE(g.shape); REQUIRE_FALSE(g.queryBorrow); REQUIRE_FALSE(g.charged); }

    auto* shape = fixture.bank.Find(fixture.names[0], false, true);
    REQUIRE(s.HoldRegisteredQueryModel(0, shape) == SimulationResidencyState::RegisteredBorrowResult::Held);
    const auto charge = s.registeredBorrowShapeCharge;
    REQUIRE(charge > 0); REQUIRE(charge <= SimulationMaxShapeCharge);
    REQUIRE(s.HoldRegisteredQueryModel(0, shape) == SimulationResidencyState::RegisteredBorrowResult::Held);
    REQUIRE(s.registeredBorrowedModels == 1); REQUIRE(s.registeredBorrowShapeCharge == charge);
    Ref<ObjectPlain> a = new ObjectPlain(shape, 1001), b = new ObjectPlain(shape, 1002);
    a->SetType(Primary); b->SetType(Primary);
    SimulationResidencyState::Entry first, second;
    first.object = a.GetRef(); first.shape = shape; second.object = b.GetRef(); second.shape = shape;
    REQUIRE(s.AttachQueryBorrow(0, first)); REQUIRE(s.AttachQueryBorrow(0, second));
    REQUIRE(s.MatchesQueryBorrow(first)); REQUIRE(s.MatchesQueryBorrow(second));
    REQUIRE(s.groups[0].queryEntries == 2);
    // A model observation does not manufacture any actor/query region Ready.
    REQUIRE(s.regions.empty()); REQUIRE(s.stats.readyRegions == 0); REQUIRE(s.stats.readyQueries == 0);
    SECTION("saved survivor retains its own CPU state after model debt retires")
    {
        b->SetDammage(.25f); REQUIRE(b->MustBeSaved());
        s.ReleaseQueryBorrow(first); REQUIRE(s.registeredBorrowedModels == 1);
        s.ReleaseQueryBorrow(second);
        REQUIRE(s.registeredBorrowedModels == 0); REQUIRE(s.registeredBorrowShapeCharge == 0);
        REQUIRE(b->GetShape() == shape); REQUIRE(b->GetRawTotalDammage() == .25f); REQUIRE(b->MustBeSaved());
    }
    SECTION("expired off-bin model blocks complete metadata despite intact selected geometry")
    {
        fixture.external.back() = nullptr;
        REQUIRE(fixture.bank.Find(fixture.names.back(), false, true) == nullptr);
        REQUIRE(s.groups.back().registeredShape.GetRef() == nullptr);
        REQUIRE(s.ValidateRegisteredMetadata(fixture.bank, fixture.names, fixture.config, [] { return true; }) == SimulationResidencyStatus::Unknown);
        REQUIRE(s.MatchesQueryBorrow(first)); REQUIRE(s.registeredBorrowedModels == 1);
    }
    SECTION("current config is reprobed rather than cached with the bank")
    {
        ParamFile changed; ParseRegisteredConfig(changed, "class CfgModels { class registered_256 { properties[]={\"class\",\"house\"}; }; };");
        REQUIRE(s.ValidateRegisteredMetadata(fixture.bank, fixture.names, changed, [] { return true; }) == SimulationResidencyStatus::Unknown);
        REQUIRE(s.ValidateRegisteredMetadata(fixture.bank, fixture.names, fixture.config, [] { return true; }) == SimulationResidencyStatus::Ready);
    }
    SECTION("expired and reloaded bank identity cannot inherit the old weak witness even with equal radius and revision")
    {
        const float radius = fixture.external.back()->BoundingSphere();
        const uint64_t revision = fixture.external.back()->QueryPolicyRevision();
        Link<LODShape> old = fixture.external.back().GetRef();
        fixture.external.back() = nullptr; REQUIRE(old.GetRef() == nullptr);
        ShapeBank::CpuOnlyLoadScope cpuOnly;
        fixture.external.back() = fixture.bank.NewFromModel(fixture.names.back(), false, true,
            std::make_shared<Model::Model>(*fixture.prototype), nullptr);
        REQUIRE(fixture.external.back()->BoundingSphere() == radius);
        REQUIRE(fixture.external.back()->QueryPolicyRevision() == revision);
        REQUIRE(old.GetRef() == nullptr);
        REQUIRE(s.ValidateRegisteredMetadata(fixture.bank, fixture.names, fixture.config, [] { return true; }) == SimulationResidencyStatus::Unknown);
        bool changed = false;
        REQUIRE(s.RefreshRegisteredModel(uint32_t(s.groups.size() - 1), fixture.bank, fixture.names.back(), fixture.config, changed, [] { return true; }) == SimulationResidencyStatus::Ready);
        REQUIRE(changed); REQUIRE(s.registeredMetadataDirty);
    }
    SECTION("same-valued off-bin revision cannot pass numeric equality")
    {
        fixture.external.back()->SetAutoCenter(fixture.external.back()->IsAutoCenter());
        REQUIRE(s.ValidateRegisteredMetadata(fixture.bank, fixture.names, fixture.config, [] { return true; }) == SimulationResidencyStatus::Unknown);
    }
    SECTION("unversioned point mutation cannot reuse an old shape/radius certificate")
    {
        auto* changed = fixture.external.back().GetRef(); const auto revision = changed->QueryPolicyRevision();
        auto* geometry = changed->GeometryLevel(); geometry->SetPos(0) = Vector3(1000000, 0, 0);
        REQUIRE(changed->QueryPolicyRevision() == revision);
        REQUIRE(s.ValidateRegisteredMetadata(fixture.bank, fixture.names, fixture.config, [] { return true; }) == SimulationResidencyStatus::Unknown);
    }
    SECTION("unversioned LOD box mutation is re-read in this owner operation")
    {
        auto* changed = fixture.external.back().GetRef(); const auto revision = changed->QueryPolicyRevision();
        changed->GeometryLevel()->MinMax()[0] = Vector3(1000000, 1000000, 1000000);
        REQUIRE(changed->QueryPolicyRevision() == revision);
        REQUIRE(s.ValidateRegisteredMetadata(fixture.bank, fixture.names, fixture.config, [] { return true; }) == SimulationResidencyStatus::Unknown);
    }
    SECTION("nonfinite query points cannot hide behind finite authored bounds")
    {
        fixture.external.back()->GeometryLevel()->SetPos(0) = Vector3(std::numeric_limits<float>::quiet_NaN(), 0, 0);
        REQUIRE(s.ValidateRegisteredMetadata(fixture.bank, fixture.names, fixture.config, [] { return true; }) == SimulationResidencyStatus::Unknown);
    }
    SECTION("undersized unversioned query LOD sphere cannot pass an otherwise valid parent sphere and box")
    {
        auto* changed = fixture.external.back().GetRef(); const auto revision = changed->QueryPolicyRevision();
        auto* geometry = changed->GeometryLevel();
        REQUIRE(geometry->BSphereRadius() > 0);
        geometry->SetMinMax(geometry->Min(), geometry->Max(), geometry->BSphereCenter(), 0);
        REQUIRE(changed->QueryPolicyRevision() == revision);
        REQUIRE(s.ValidateRegisteredMetadata(fixture.bank, fixture.names, fixture.config, [] { return true; }) == SimulationResidencyStatus::Unknown);
    }
    SECTION("shifted unversioned query LOD sphere cannot pass merely because its radius is unchanged")
    {
        auto* changed = fixture.external.back().GetRef(); const auto revision = changed->QueryPolicyRevision();
        auto* geometry = changed->GeometryLevel();
        geometry->SetMinMax(geometry->Min(), geometry->Max(), Vector3(1000000, 0, 0), geometry->BSphereRadius());
        REQUIRE(changed->QueryPolicyRevision() == revision);
        REQUIRE(s.ValidateRegisteredMetadata(fixture.bank, fixture.names, fixture.config, [] { return true; }) == SimulationResidencyStatus::Unknown);
    }
    SECTION("unversioned invalid query face references refuse before the legacy narrow phase")
    {
        auto* changed = fixture.external.back().GetRef(); const auto revision = changed->QueryPolicyRevision();
        auto* geometry = changed->GeometryLevel();
        geometry->Face(geometry->BeginFaces()).Set(0, geometry->NPos());
        REQUIRE(changed->QueryPolicyRevision() == revision);
        REQUIRE(s.ValidateRegisteredMetadata(fixture.bank, fixture.names, fixture.config, [] { return true; }) == SimulationResidencyStatus::Unknown);
    }
    SECTION("unversioned query vertex conform flags cannot borrow static coverage")
    {
        auto* changed = fixture.external.back().GetRef(); const auto revision = changed->QueryPolicyRevision();
        auto* geometry = changed->GeometryLevel();
        geometry->SetClip(0, geometry->Clip(0) | ClipLandKeep);
        REQUIRE(changed->QueryPolicyRevision() == revision);
        REQUIRE(s.ValidateRegisteredMetadata(fixture.bank, fixture.names, fixture.config, [] { return true; }) == SimulationResidencyStatus::Unknown);
    }
    SECTION("bounded validation cannot turn a valid prefix into completeness")
    {
        size_t visits = 0;
        REQUIRE(s.ValidateRegisteredMetadata(fixture.bank, fixture.names, fixture.config, [&] { return ++visits < 50; }) == SimulationResidencyStatus::Pending);
        REQUIRE(s.registeredValidationBudgetRefusals > 0);
    }
    SECTION("watch overflow remains a complete-query refusal")
    {
        s.watchCapacityExceeded = true;
        REQUIRE(s.ValidateRegisteredMetadata(fixture.bank, fixture.names, fixture.config, [] { return true; }) == SimulationResidencyStatus::CapacityExceeded);
    }
}

TEST_CASE("Registered model borrowing reserves the combined union before factory work and rolls back unused holds", "[streaming-simulation-residency]")
{
    RegisteredPlainInventory fixture(SimulationMaxModels + 1); auto& s = fixture.state;
    REQUIRE(s.AdvanceRegisteredPadding(fixture.bank, fixture.names, fixture.config, [] { return true; }) == SimulationResidencyStatus::Ready);
    auto* shape = fixture.external[0].GetRef();
    SECTION("normal and cold ownership use the same 256-model union")
    {
        s.legacyShapeSlots = SimulationMaxModels - 1; s.positiveColdWork.push_back(1);
        REQUIRE(s.HoldRegisteredQueryModel(0, shape) == SimulationResidencyState::RegisteredBorrowResult::CapacityExceeded);
        REQUIRE_FALSE(s.groups[0].queryBorrow); REQUIRE(s.registeredBorrowShapeCharge == 0);
    }
    SECTION("shape scheduling cannot ignore existing legacy/cold debt")
    {
        s.stats.shapeSchedulingCharge = SimulationMaxShapeCharge;
        REQUIRE(s.HoldRegisteredQueryModel(0, shape) == SimulationResidencyState::RegisteredBorrowResult::CapacityExceeded);
        REQUIRE_FALSE(s.groups[0].queryBorrow); REQUIRE(s.registeredBorrowedModels == 0);
    }
    SECTION("failed factory has no entry and releases its reserved debt")
    {
        REQUIRE(s.HoldRegisteredQueryModel(0, shape) == SimulationResidencyState::RegisteredBorrowResult::Held);
        REQUIRE(s.groups[0].queryEntries == 0);
        s.ReleaseUnusedRegisteredBorrow(0); s.ReleaseUnusedRegisteredBorrow(0);
        REQUIRE_FALSE(s.groups[0].queryBorrow); REQUIRE(s.registeredBorrowedModels == 0); REQUIRE(s.registeredBorrowShapeCharge == 0);
        REQUIRE(fixture.bank.Find(fixture.names[0], false, true) == shape);
    }
    SECTION("inherited current config and long alias names are unsupported")
    {
        ParamFile inherited; ParseRegisteredConfig(inherited, "class Base { properties[]={\"class\",\"house\"}; }; class CfgModels { class registered_0: Base {}; };");
        LODShapeWithShadow* found = nullptr;
        REQUIRE(SimulationResidencyState::ProbeRegisteredPlainNow(fixture.bank, fixture.names[0], inherited, found, [] { return true; }) == SimulationResidencyStatus::Unknown);
        REQUIRE(found == nullptr);
        const std::string prefix(127, 'a');
        const std::string alias = prefix + "X.p3d", collision = prefix + "Y.p3d";
        ShapeBank::CpuOnlyLoadScope cpuOnly;
        Ref<LODShapeWithShadow> cached = fixture.bank.NewFromModel(alias.c_str(), false, true,
            std::make_shared<Model::Model>(*fixture.prototype), nullptr);
        REQUIRE(fixture.bank.Find(collision.c_str(), false, true) == cached.GetRef()); // real 128-byte bank-key alias
        REQUIRE(SimulationResidencyState::ProbeRegisteredPlainNow(fixture.bank, alias.c_str(), fixture.config, found, [] { return true; }) == SimulationResidencyStatus::Unknown);
        REQUIRE(SimulationResidencyState::ProbeRegisteredPlainNow(fixture.bank, collision.c_str(), fixture.config, found, [] { return true; }) == SimulationResidencyStatus::Unknown);
    }
    SECTION("huge off-bin geometry cannot retain the previous small-radius certificate")
    {
        Matrix4 large = MIdentity; large.SetScale(1000000);
        fixture.external.back()->InternalTransform(large);
        REQUIRE(s.ValidateRegisteredMetadata(fixture.bank, fixture.names, fixture.config, [] { return true; }) == SimulationResidencyStatus::Unknown);
    }
}
