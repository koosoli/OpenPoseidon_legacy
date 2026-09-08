#include <catch2/catch_test_macros.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/World/Entities/Vehicles/Misc/Ship.hpp>
#include <Poseidon/World/Entities/Vehicles/Misc/BoatWaterResponse.hpp>
#include <catch2/catch_approx.hpp>

TEST_CASE("ship.hpp compiles", "[vehicles][ship]")
{
    SUCCEED("header included successfully");
}

TEST_CASE("Planing drag preserves flat water and bounds swell draft", "[vehicles][ship][waves]")
{
    using Poseidon::PlaningWaterDragDepth;
    for (float draft : {0.0f, 0.02f, 0.5f, 2.0f})
        CHECK(PlaningWaterDragDepth(draft, draft) == draft);
    CHECK(PlaningWaterDragDepth(2.0f, 0.05f) == Catch::Approx(0.30f));
    CHECK(PlaningWaterDragDepth(8.0f, 0.05f) == Catch::Approx(0.30f));
    CHECK(PlaningWaterDragDepth(0.1f, 0.5f) == Catch::Approx(0.1f));
    CHECK(PlaningWaterDragDepth(-0.1f, 0.5f) == 0.0f);
    CHECK(PlaningWaterDragDepth(1.0f, -1.0f) == Catch::Approx(0.25f));
}
