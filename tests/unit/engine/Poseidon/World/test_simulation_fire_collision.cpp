#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/SimulationFireCollision.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <vector>

using namespace Poseidon;
using namespace Poseidon::Streaming;

TEST_CASE("Simulation fire float domain refuses degenerate and unsafe legacy conversions", "[streaming-simulation-fire]")
{
    using Status = SimulationResidencyStatus;
    REQUIRE(SimulationFireBroadphaseStatus(Vector3(1190,11.5f,1200), Vector3(1210,11.5f,1200), .01f, .02f, 32) == Status::Ready);
    REQUIRE(SimulationFireBroadphaseStatus(VZero, VZero, 0, .02f, 32) == Status::Invalid);
    REQUIRE(SimulationFireBroadphaseStatus(VZero, Vector3(.0009f,0,0), 0, .02f, 32) == Status::Invalid);
    REQUIRE(SimulationFireBroadphaseStatus(VZero, Vector3(.0011f,0,0), 0, .02f, 32) == Status::Ready);
    const float infinity = std::numeric_limits<float>::infinity();
    REQUIRE(SimulationFireBroadphaseStatus(VZero, Vector3(infinity,0,0), 0, .02f, 32) == Status::Invalid);
    REQUIRE(SimulationFireBroadphaseStatus(Vector3(1e17f,0,0), Vector3(-1e17f,0,0), 0, .02f, 32) == Status::Invalid);
    REQUIRE(SimulationFireBroadphaseStatus(VZero, Vector3(1,0,0), 1e17f, .02f, 32) == Status::Invalid);
    REQUIRE(SimulationFireBroadphaseStatus(VZero, Vector3(1,0,0), 0, infinity, 32) == Status::Invalid);
    REQUIRE(SimulationFireBroadphaseStatus(VZero, Vector3(1,0,0), 0, 0, 32) == Status::Invalid);
    REQUIRE(SimulationFireBroadphaseStatus(VZero, Vector3(1,0,0), 0, .02f, 0) == Status::Invalid);
}

TEST_CASE("Simulation fire bounds actual clamped legacy rectangle and int rounding", "[streaming-simulation-fire]")
{
    using Status = SimulationResidencyStatus;
    // 0..63 on both axes: exactly4096 cells. One extra column refuses even
    // though a thin diagonal planner capsule can remain below its cell cap.
    REQUIRE(SimulationFireBroadphaseStatus(Vector3(25,0,25), Vector3(3150,0,3150), 0, .02f, 512) == Status::Ready);
    REQUIRE(SimulationFireBroadphaseStatus(Vector3(25,0,25), Vector3(3200,0,3150), 0, .02f, 512) == Status::CapacityExceeded);
    // Legacy toIntFloor(1) rounds .5 to even0, adding the preceding cell.
    // Mathematical floor incorrectly counts64x64 instead of the real65x65.
    REQUIRE(SimulationFireBroadphaseStatus(Vector3(75,0,75), Vector3(3200,0,3200), 0, .02f, 512) == Status::CapacityExceeded);
    REQUIRE(SimulationFireBroadphaseStatus(Vector3(25,0,25), Vector3(25000,0,25000), 0, .02f, 512) == Status::CapacityExceeded);
    // Out-of-world coordinates clamp to one real cell, but only after safe
    // integer conversion. Finite coordinates near int32's float round-up must
    // stay outside this wrapper's conservative numeric admission domain.
    REQUIRE(SimulationFireBroadphaseStatus(Vector3(-1000,0,-1000), Vector3(-900,0,-900), 0, .02f, 512) == Status::Ready);
    REQUIRE(SimulationFireBroadphaseStatus(Vector3(2147483648.f,0,0), Vector3(2147483904.f,0,0), 0, 1, 32) == Status::Invalid);
    REQUIRE(SimulationFireBroadphaseStatus(Vector3(2147475456.f,0,0), Vector3(2147475712.f,0,0), 0, 1, 32) == Status::Ready);
}

namespace
{
void Contact(CollisionBuffer& buffer, Object* object, int component)
{
    CollisionInfo contact{};
    contact.object = object;
    contact.component = component;
    buffer.Add(contact);
}
struct Objects
{
    Ref<LODShapeWithShadow> shape = new LODShapeWithShadow;
    Ref<ObjectPlain> nearWall, offsetWall, dynamic, excluded;
    Objects()
    {
        Foundation::CaptureMainThread();
        nearWall = new ObjectPlain(shape, 1001);
        offsetWall = new ObjectPlain(shape, 1002);
        dynamic = new ObjectPlain(shape, 2000);
        excluded = new ObjectPlain(shape, 2001);
        for (Object* object : {nearWall.GetRef(), offsetWall.GetRef(), dynamic.GetRef(), excluded.GetRef()})
        {
            // Object's empty-shape constructor starts Temporary. Production
            // plain placement admission assigns Primary before eligibility.
            object->SetType(Primary);
            object->SetTransform(MIdentity);
        }
        // The supplementary trace must not impose the legacy 25m origin margin.
        offsetWall->SetPosition(Vector3(100, 0, 100));
    }
};
}

TEST_CASE("Simulation fire merge preserves original contacts and completes offset terrain without duplicate component sets", "[streaming-simulation-fire]")
{
    Objects objects;
    CollisionBuffer result;
    Contact(result, objects.nearWall, 99); // Existing caller prefix must survive.
    std::vector<Object*> candidates{objects.nearWall.GetRef(), objects.offsetWall.GetRef(), objects.offsetWall.GetRef()};
    int legacyCalls = 0, terrainCalls = 0;
    const auto status = AppendSimulationFireContacts(result, candidates, nullptr, nullptr,
        [&](CollisionBuffer& staged) {
            ++legacyCalls;
            Contact(staged, objects.dynamic, 7);
            Contact(staged, objects.nearWall, 1);
            Contact(staged, objects.nearWall, 2);
        },
        [&](CollisionBuffer& staged, Object& object) {
            ++terrainCalls;
            REQUIRE(&object == objects.offsetWall.GetRef());
            REQUIRE(object.Position().X() > 25);
            Contact(staged, &object, 3);
            Contact(staged, &object, 4);
        }, [] {return SimulationResidencyStatus::Ready;}, [] {return true;});
    REQUIRE(status == SimulationResidencyStatus::Ready);
    REQUIRE(legacyCalls == 1);
    REQUIRE(terrainCalls == 1);
    REQUIRE(result.Size() == 6);
    REQUIRE(result[0].object.GetRef() == objects.nearWall.GetRef());
    REQUIRE(result[0].component == 99);
    REQUIRE(result[1].object.GetRef() == objects.dynamic.GetRef());
    REQUIRE(result[1].component == 7);
    REQUIRE(result[2].component == 1);
    REQUIRE(result[3].component == 2);
    REQUIRE(result[4].component == 3);
    REQUIRE(result[5].component == 4);
}

TEST_CASE("A terrain instance queried but missed by legacy is still directly intersected", "[streaming-simulation-fire]")
{
    Objects objects;
    CollisionBuffer result;
    std::vector<Object*> candidates{objects.nearWall.GetRef()};
    bool legacyQueried = false, directQueried = false;
    const auto status = AppendSimulationFireContacts(result, candidates, nullptr, nullptr,
        [&](CollisionBuffer&) {legacyQueried = true;},
        [&](CollisionBuffer& staged, Object& object) {directQueried = true;Contact(staged, &object, 1);},
        [] {return SimulationResidencyStatus::Ready;}, [] {return true;});
    REQUIRE(status == SimulationResidencyStatus::Ready);
    REQUIRE(legacyQueried);
    REQUIRE(directQueried);
    REQUIRE(result.Size() == 1);
}

TEST_CASE("Simulation fire merge honours top-level and child exclusions without touching caller prefix", "[streaming-simulation-fire]")
{
    Objects objects;
    CollisionBuffer result;
    Contact(result, objects.excluded, 99);
    std::vector<Object*> candidates{objects.excluded.GetRef(), objects.nearWall.GetRef()};
    int calls = 0;
    const auto status = AppendSimulationFireContacts(result, candidates, objects.excluded, objects.dynamic,
        [&](CollisionBuffer& staged) {Contact(staged, objects.dynamic, 1);},
        [&](CollisionBuffer& staged, Object& object) {
            ++calls;
            REQUIRE(&object == objects.nearWall.GetRef());
            Contact(staged, &object, 2);
            Contact(staged, objects.excluded, 3); // Child exclusion.
        }, [] {return SimulationResidencyStatus::Ready;}, [] {return true;});
    REQUIRE(status == SimulationResidencyStatus::Ready);
    REQUIRE(calls == 1);
    REQUIRE(result.Size() == 2);
    REQUIRE(result[0].component == 99);
    REQUIRE(result[1].object.GetRef() == objects.nearWall.GetRef());
}

TEST_CASE("Simulation fire work refusal or changed certificate never publishes a partial contact prefix", "[streaming-simulation-fire]")
{
    Objects objects;
    CollisionBuffer result;
    Contact(result, objects.dynamic, 99);
    std::vector<Object*> candidates{objects.nearWall.GetRef(), objects.offsetWall.GetRef()};
    int budgetChecks = 0, validations = 0, terrainCalls = 0;
    int limit = 100;
    auto finalStatus = SimulationResidencyStatus::Ready;
    SECTION("budget exhausted after legacy") {limit = 1;}
    SECTION("budget exhausted after first terrain candidate") {limit = 3;}
    SECTION("final validation changed") {finalStatus = SimulationResidencyStatus::Unknown;}
    SECTION("null live instance") {candidates[1] = nullptr;}
    const auto status = AppendSimulationFireContacts(result, candidates, nullptr, nullptr,
        [&](CollisionBuffer& staged) {Contact(staged, objects.dynamic, 1);},
        [&](CollisionBuffer& staged, Object& object) {++terrainCalls;Contact(staged, &object, 2);},
        [&] {return ++validations == 1 ? SimulationResidencyStatus::Ready : finalStatus;},
        [&] {return ++budgetChecks <= limit;});
    REQUIRE(status != SimulationResidencyStatus::Ready);
    REQUIRE(result.Size() == 1);
    REQUIRE(result[0].object.GetRef() == objects.dynamic.GetRef());
    REQUIRE(result[0].component == 99);
    if (limit == 3) REQUIRE(terrainCalls == 1);
}

TEST_CASE("Simulation fire nonReady and complete-set capacity refusal perform no collision work", "[streaming-simulation-fire]")
{
    Objects objects;
    CollisionBuffer result;
    Contact(result, objects.dynamic, 99);
    std::vector<Object*> candidates{objects.nearWall.GetRef()};
    auto readiness = SimulationResidencyStatus::Pending;
    SECTION("Pending") {}
    SECTION("Unknown") {readiness = SimulationResidencyStatus::Unknown;}
    SECTION("Disabled") {readiness = SimulationResidencyStatus::Disabled;}
    SECTION("WrongOwner") {readiness = SimulationResidencyStatus::WrongOwner;}
    SECTION("over capacity") {readiness = SimulationResidencyStatus::Ready;candidates.resize(SimulationMaxRegionLeases + 1, objects.nearWall.GetRef());}
    int calls = 0;
    const auto status = AppendSimulationFireContacts(result, candidates, nullptr, nullptr,
        [&](CollisionBuffer&) {++calls;}, [&](CollisionBuffer&, Object&) {++calls;},
        [&] {return readiness;}, [] {return true;});
    REQUIRE(status == (readiness == SimulationResidencyStatus::Ready ? SimulationResidencyStatus::CapacityExceeded : readiness));
    REQUIRE(calls == 0);
    REQUIRE(result.Size() == 1);
    REQUIRE(result[0].component == 99);
}

TEST_CASE("Empty simulation fire output never turns refused coverage into a Ready clear ray", "[streaming-simulation-fire]")
{
    Objects objects;
    CollisionBuffer result;
    std::vector<Object*> candidates{objects.nearWall.GetRef()};
    auto readiness = SimulationResidencyStatus::Pending;
    SECTION("Pending") {}
    SECTION("Unknown") {readiness = SimulationResidencyStatus::Unknown;}
    SECTION("Invalid") {readiness = SimulationResidencyStatus::Invalid;}
    SECTION("WrongOwner") {readiness = SimulationResidencyStatus::WrongOwner;}
    SECTION("CapacityExceeded") {readiness = SimulationResidencyStatus::CapacityExceeded;}
    int calls = 0;
    const auto status = AppendSimulationFireContacts(result, candidates, nullptr, nullptr,
        [&](CollisionBuffer&) {++calls;}, [&](CollisionBuffer&, Object&) {++calls;},
        [&] {return readiness;}, [] {return true;});
    REQUIRE(status == readiness);
    REQUIRE(status != SimulationResidencyStatus::Ready);
    REQUIRE(result.Size() == 0);
    REQUIRE(calls == 0);
}
