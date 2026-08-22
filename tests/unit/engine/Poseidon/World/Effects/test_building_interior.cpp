#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Poseidon/World/Effects/BuildingInterior.hpp>
#include <Poseidon/World/Weather/RainVolume.hpp>

#include <utility>
#include <vector>

using namespace Poseidon;

namespace Poseidon
{

struct BuildingInteriorTestAccess
{
    static BuildingInterior MakeColumn(std::vector<unsigned char> labels)
    {
        BuildingInterior interior;
        interior._valid = true;
        interior._roomCount = 1;
        interior._gridMin = VZero;
        interior._cell = 0.25f;
        interior._dim[0] = 1;
        interior._dim[1] = static_cast<int>(labels.size());
        interior._dim[2] = 1;
        interior._labels = std::move(labels);
        return interior;
    }

    static BuildingInterior MakeRoomBox(int dimension)
    {
        BuildingInterior interior;
        interior._valid = true;
        interior._roomCount = 1;
        interior._gridMin = VZero;
        interior._cell = 0.25f;
        interior._dim[0] = dimension;
        interior._dim[1] = dimension;
        interior._dim[2] = dimension;
        interior._labels.assign(static_cast<size_t>(dimension * dimension * dimension), 0);
        return interior;
    }

    static void AddPortal(BuildingInterior& interior, const InteriorPortal& portal)
    {
        interior._portals.Add(portal);
    }
};

} // namespace Poseidon

TEST_CASE("Building interior segment classification cannot tunnel through a one-cell roof",
          "[BuildingInterior][Rain][Weather][World]")
{
    // Bottom two cells are room 0, the middle cell is solid shell, and the top
    // two are outside air. A single endpoint lookup would see only outside at
    // both ends; the segment classifier must still observe the roof.
    BuildingInterior interior = BuildingInteriorTestAccess::MakeColumn({0, 0, 0xFE, 0xFF, 0xFF});

    REQUIRE(interior.ClassifyModel(Vector3(0.125f, 1.125f, 0.125f)) == -1);
    REQUIRE(interior.ClassifyModel(Vector3(0.125f, 0.125f, 0.125f)) == 0);
    REQUIRE(interior.ClassifySegmentModel(Vector3(0.125f, 1.125f, 0.125f),
                                          Vector3(0.125f, 0.125f, 0.125f)) == -2);
}

TEST_CASE("Rain may cross a shell where a portal opening sits, and only there",
          "[BuildingInterior][Rain][Weather][World]")
{
    // Room below, solid shell in the middle, outside air above -- a wall with
    // a door. Without the portal the crossing is fatal; with one centred on
    // the shell cell, points inside its radius are openings and points far
    // away are not.
    BuildingInterior interior = BuildingInteriorTestAccess::MakeColumn({0, 0, 0xFE, 0xFF, 0xFF});

    REQUIRE(interior.ClassifySegmentModel(Vector3(0.125f, 0.875f, 0.125f),
                                          Vector3(0.125f, 0.375f, 0.125f)) == -2);

    InteriorPortal door;
    door.centre = Vector3(0.125f, 0.625f, 0.125f);
    door.radius = 0.9f;
    door.room = 0;
    BuildingInteriorTestAccess::AddPortal(interior, door);

    REQUIRE(interior.PointInPortalModel(Vector3(0.125f, 0.625f, 0.125f)));   // dead centre of the door
    REQUIRE(interior.PointInPortalModel(Vector3(0.125f, 0.75f, 0.125f)));    // within the radius
    REQUIRE_FALSE(interior.PointInPortalModel(Vector3(0.125f, 2.5f, 0.125f))); // high above it: still wall
    REQUIRE_FALSE(interior.PointInPortalModel(Vector3(5.0f, 0.625f, 5.0f)));   // far to the side
}

TEST_CASE("Building interior movement cannot turn a solid wall into an implicit portal",
          "[BuildingInterior][Smoke][World]")
{
    BuildingInterior interior = BuildingInteriorTestAccess::MakeColumn({0, 0, 0xFE, 0xFF});
    const InteriorStepResult step = interior.ContainedStepModel(Vector3(0.125f, 0.125f, 0.125f),
                                                                Vector3(0.125f, 0.875f, 0.125f), 0);

    REQUIRE(step.blocked);
    REQUIRE(step.room == 0);
    REQUIRE(step.position.Y() < 0.5f);
}

TEST_CASE("Building interior clearance bounds an indoor smoke billboard",
          "[BuildingInterior][Smoke][World]")
{
    BuildingInterior interior = BuildingInteriorTestAccess::MakeRoomBox(9);

    const float centre = interior.ClearanceModel(Vector3(1.125f, 1.125f, 1.125f), 0, 5.0f);
    const float nearRoof = interior.ClearanceModel(Vector3(1.125f, 2.0f, 1.125f), 0, 5.0f);

    REQUIRE(centre > 0.85f);
    REQUIRE(centre < 1.05f);
    REQUIRE(nearRoof > 0.10f);
    REQUIRE(nearRoof < 0.30f);
}

TEST_CASE("Particle rain follows wind speed and direction and relaxes after a gust change",
          "[Rain][Weather][Wind]")
{
    const Vector3 east = RainTargetVelocity(10.0f, 8.0f, 0.0f, 0.75f);
    const Vector3 north = RainTargetVelocity(10.0f, 0.0f, 12.0f, 0.75f);

    REQUIRE(east.X() == Catch::Approx(6.0f));
    REQUIRE(east.Y() == Catch::Approx(-10.0f));
    REQUIRE(east.Z() == Catch::Approx(0.0f));
    REQUIRE(north.X() == Catch::Approx(0.0f));
    REQUIRE(north.Z() == Catch::Approx(9.0f));

    const Vector3 afterOneTimeConstant = RainRelaxVelocity(Vector3(6.0f, -10.0f, 0.0f), north, 5.0f, 0.2f);
    REQUIRE(afterOneTimeConstant.X() < 3.0f);
    REQUIRE(afterOneTimeConstant.Z() > 5.0f);
    REQUIRE(afterOneTimeConstant.Y() == Catch::Approx(-10.0f));

    const Vector3 windIgnored = RainTargetVelocity(10.0f, 8.0f, 12.0f, 0.0f);
    REQUIRE(windIgnored.X() == Catch::Approx(0.0f));
    REQUIRE(windIgnored.Z() == Catch::Approx(0.0f));
}
