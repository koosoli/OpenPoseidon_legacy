#include <catch2/catch_test_macros.hpp>

#include <Poseidon/World/Effects/BuildingInterior.hpp>

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
