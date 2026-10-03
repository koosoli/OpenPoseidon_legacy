#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Dev/Diag/CaveEditor.hpp>
#include <limits>

TEST_CASE("Excavation footprint rotation keeps the entrance fixed and checks the proposed occupant",
          "[CaveEditor][geometry]")
{
    Poseidon::Dev::EditorCaveInfo old;
    old.x = 100;
    old.z = 200;
    old.width = 2;
    old.length = 12;
    REQUIRE(Poseidon::Dev::EditorCaveFootprintContains(old, 100, 201));
    REQUIRE_FALSE(Poseidon::Dev::EditorCaveFootprintContains(old, 100, 198));
    auto candidate = old;
    candidate.heading = 180;
    REQUIRE(Poseidon::Dev::EditorCaveFootprintContains(candidate, 100, 198));
    REQUIRE_FALSE(Poseidon::Dev::EditorCaveFootprintContains(candidate, 100, 201));
    REQUIRE(Poseidon::Dev::EditorCaveFootprintContains(candidate, 100, 200));
    candidate.heading = 90;
    REQUIRE(Poseidon::Dev::EditorCaveFootprintContains(candidate, 105, 200));
    REQUIRE_FALSE(Poseidon::Dev::EditorCaveFootprintContains(candidate, 100, 205));
    candidate.heading = 450;
    REQUIRE(Poseidon::Dev::EditorCaveFootprintContains(candidate, 105, 200));
    REQUIRE_FALSE(Poseidon::Dev::EditorCaveFootprintContains(candidate, 100, 205));
}

TEST_CASE("Excavation overlap catches crossing edges and full containment", "[CaveEditor][geometry]")
{
    Poseidon::Dev::EditorCaveInfo a;
    a.x = 100;
    a.z = 200;
    a.width = 2;
    a.length = 12;
    auto b = a;
    b.x = 97;
    b.z = 206;
    b.heading = 90;
    b.length = 6;
    REQUIRE(Poseidon::Dev::EditorCaveFootprintsOverlap(a, b)); // cross without a contained rectangle corner
    REQUIRE(Poseidon::Dev::EditorCaveFootprintsOverlap(b, a));
    b = a;
    b.width = .5f;
    b.length = 3;
    b.z = 203;
    REQUIRE(Poseidon::Dev::EditorCaveFootprintsOverlap(a, b));
    b = a;
    b.x = 104;
    REQUIRE_FALSE(Poseidon::Dev::EditorCaveFootprintsOverlap(a, b));
    b = a;
    b.x = 102;
    REQUIRE(Poseidon::Dev::EditorCaveFootprintsOverlap(a, b)); // shared boundary is occupied
}

TEST_CASE("Excavation support guard handles elevated tank origins and bounded cave roofs", "[CaveEditor][occupancy]")
{
    using namespace Poseidon::Dev;
    EditorCaveInfo trench;
    trench.tool = ExcavationTool::TankTrench;
    trench.y = 100;
    trench.height = 2;
    const float originY = 100.46f, trackMinimumY = 98;
    REQUIRE(originY > 100); // origin-only terrain test would miss the parked tank
    REQUIRE(EditorCaveSupportOccupied(trench, trackMinimumY, 100));
    REQUIRE_FALSE(EditorCaveSupportOccupied(trench, 101, 100));
    REQUIRE(EditorCaveSupportOccupied(trench, 100, 100, true)); // candidate cannot excavate under ground-level actor
    REQUIRE_FALSE(EditorCaveSupportOccupied(trench, 100, 100)); // ordinary surface origin alone is not underground
    EditorCaveInfo cave;
    cave.y = 100;
    cave.height = 2.5f;
    REQUIRE(EditorCaveSupportOccupied(cave, 100, 105));
    REQUIRE_FALSE(EditorCaveSupportOccupied(cave, 103, 105)); // above bounded roof remains safe to delete
    REQUIRE(EditorCaveSupportOccupied(cave, 100, 105, true));
    REQUIRE_FALSE(EditorCaveSupportOccupied(cave, 103, 105, true));
    REQUIRE(EditorCaveSupportOccupied(cave, std::numeric_limits<float>::quiet_NaN(), 105));
}
