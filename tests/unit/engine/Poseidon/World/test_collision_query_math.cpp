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
