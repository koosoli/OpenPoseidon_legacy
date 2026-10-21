#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Simulation/CollisionQueryMath.hpp>

using namespace Poseidon;

TEST_CASE("Swept query broadphase includes query thickness", "[collision-query]")
{
    REQUIRE(SweptSegmentBoundingRadius(8, 0) == 4);
    REQUIRE(SweptSegmentBoundingRadius(0, 0.25f) == 0.25f);
    REQUIRE(SweptSegmentBoundingRadius(8, 0.25f) == 4.25f);
    REQUIRE(SweptSegmentBoundingRadius(0, 0) == 0);

    // An obstacle beyond a segment endpoint, but touching its rounded cap,
    // must reach narrowphase. The old length/2 bound rejected this contact.
    const float obstacleRadius = 0.125f;
    const float obstacleCentreDistance = 4.375f;
    REQUIRE(obstacleCentreDistance <= SweptSegmentBoundingRadius(8, 0.25f) + obstacleRadius);
    REQUIRE(obstacleCentreDistance > SweptSegmentBoundingRadius(8, 0) + obstacleRadius);
}

TEST_CASE("Segment-point distance clamps to the segment", "[collision-query]")
{
    // Beside the middle, past either end, and on a degenerate segment.
    REQUIRE(SegmentPointDistance2(0, 0, 0, 10, 0, 0, 5, 3, 0) == 9);
    REQUIRE(SegmentPointDistance2(0, 0, 0, 10, 0, 0, -4, 3, 0) == 25);
    REQUIRE(SegmentPointDistance2(0, 0, 0, 10, 0, 0, 13, 0, 4) == 25);
    REQUIRE(SegmentPointDistance2(2, 2, 2, 2, 2, 2, 2, 5, 6) == 25);
}

TEST_CASE("A line query skips a cell only beyond its static reach", "[collision-query]")
{
    // The bound Object::Intersect enforces per object: centre within 2 * radius of the
    // line. A cell's reach is max(|pos - centre| + 2 * radius); an object sitting exactly
    // on that reach must still be walked, so a cell at that distance is not skipped.
    const float offset = 3.0f, radius = 4.0f;
    const float reach = offset + 2 * radius;
    REQUIRE_FALSE(LineQueryMaySkipCell(reach * reach, reach));
    REQUIRE_FALSE(LineQueryMaySkipCell(0, reach));
    REQUIRE(LineQueryMaySkipCell((reach + 1) * (reach + 1), reach));
    // An empty reach (no static objects) skips everything farther than the slack.
    REQUIRE(LineQueryMaySkipCell(1, 0));
}
