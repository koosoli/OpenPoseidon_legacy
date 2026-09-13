#include <catch2/catch_test_macros.hpp>

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/World/Entities/Vehicles/Ground/Tank.hpp>
#include <Poseidon/World/Entities/Vehicles/Ground/TrackedWaterResponse.hpp>
#include <catch2/catch_approx.hpp>

TEST_CASE("tank.hpp compiles", "[vehicles][tank]")
{
    SUCCEED("header included successfully");
}

TEST_CASE("amphibious tracked buoyancy follows mean draft without swell catapult", "[vehicles][tank][waves]")
{
    using Poseidon::AmphibiousTrackedImmersion;

    CHECK(AmphibiousTrackedImmersion(0.0f, 0.0f) == 0.0f);
    CHECK(AmphibiousTrackedImmersion(0.8f, 0.8f) == Catch::Approx(0.8f));
    CHECK(AmphibiousTrackedImmersion(3.0f, 0.8f) == Catch::Approx(1.15f));
    CHECK(AmphibiousTrackedImmersion(-1.0f, 0.8f) == Catch::Approx(0.45f));
    CHECK(AmphibiousTrackedImmersion(1.0f, -0.5f) == 0.0f);
    CHECK(AmphibiousTrackedImmersion(0.2f, -0.1f) == Catch::Approx(0.2f));
}

TEST_CASE("amphibious tracked propulsion retains authority in open water", "[vehicles][tank][water-drive]")
{
    using Poseidon::AmphibiousTrackedPropulsionScale;

    CHECK(AmphibiousTrackedPropulsionScale(true, false, false) == Catch::Approx(0.16f));
    CHECK(AmphibiousTrackedPropulsionScale(false, false, false) == Catch::Approx(0.1f));
    CHECK(AmphibiousTrackedPropulsionScale(true, true, false) == Catch::Approx(1.0f));
    CHECK(AmphibiousTrackedPropulsionScale(true, false, true) == Catch::Approx(1.0f));
}
