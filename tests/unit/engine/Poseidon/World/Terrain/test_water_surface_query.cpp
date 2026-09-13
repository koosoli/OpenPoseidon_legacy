#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/WaterSurfaceQuery.hpp>

#include <cmath>

using namespace Poseidon;

TEST_CASE("Water surface query is deterministic and sea-level relative", "[World][Terrain][Water]")
{
    const WaterSurfaceSample first = QueryWaterSurface(123.25f, -47.5f, 18.0f, 4.0f);
    const WaterSurfaceSample second = QueryWaterSurface(123.25f, -47.5f, 18.0f, 4.0f);
    const WaterSurfaceSample raisedSea = QueryWaterSurface(123.25f, -47.5f, 18.0f, 9.0f);

    REQUIRE(first.height == second.height);
    REQUIRE(first.normalX == second.normalX);
    REQUIRE(first.velocityZ == second.velocityZ);
    REQUIRE(raisedSea.height == Catch::Approx(first.height + 5.0f));
}

TEST_CASE("Water surface query returns a normalized finite surface frame", "[World][Terrain][Water]")
{
    const WaterSurfaceSample sample = QueryWaterSurface(-918.0f, 361.0f, 27.5f, 0.0f);
    const float normalLength =
        std::sqrt(sample.normalX * sample.normalX + sample.normalY * sample.normalY + sample.normalZ * sample.normalZ);

    REQUIRE(std::isfinite(sample.height));
    REQUIRE(std::isfinite(sample.velocityX));
    REQUIRE(std::isfinite(sample.velocityZ));
    REQUIRE(normalLength == Catch::Approx(1.0f).margin(0.0001f));
    REQUIRE(sample.normalY > 0.0f);
    REQUIRE(sample.roughness >= 0.0f);
}

TEST_CASE("Mean-submerged hull contacts survive a wave trough", "[World][Terrain][Water][buoyancy]")
{
    CHECK(ShouldReportWaterContact(0.4f, 0.2f));
    CHECK(ShouldReportWaterContact(-0.4f, 0.2f));
    CHECK(ShouldReportWaterContact(0.2f, -0.4f));
    CHECK_FALSE(ShouldReportWaterContact(0.0f, 0.0f));
    CHECK_FALSE(ShouldReportWaterContact(-0.2f, -0.1f));
}
