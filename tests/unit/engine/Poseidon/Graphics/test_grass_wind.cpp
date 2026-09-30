#include <Poseidon/Graphics/Rendering/GrassWind.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

using Poseidon::render::grass::GustScrollForWind;

TEST_CASE("Grass gust fronts follow wind speed across bend amplitudes", "[Graphics][GrassWind]")
{
    // Cover the calm baseline, normal wind and weather envelope. Bend amplitude
    // is an artistic control and must not speed up the travelling gust field.
    for (const float speed : {1.5f, 6.0f, 13.0f})
        for (const float strength : {0.0f, 0.3f, 1.2f, 3.0f})
            CHECK((30.0f + 15.0f * strength) * GustScrollForWind(speed, strength)
                  == Catch::Approx(speed));
}

TEST_CASE("Grass wind scroll remains bounded in calm air and extreme inputs", "[Graphics][GrassWind]")
{
    CHECK(GustScrollForWind(0.0f, 0.0f) == Catch::Approx(0.02f));
    CHECK(GustScrollForWind(-1.0f, 0.0f) == Catch::Approx(0.02f));
    CHECK(GustScrollForWind(1000.0f, 3.0f) == Catch::Approx(1.0f));
    CHECK(GustScrollForWind(6.0f, -1.0f) == GustScrollForWind(6.0f, 0.0f));
    CHECK(GustScrollForWind(6.0f, 10.0f) == GustScrollForWind(6.0f, 3.0f));
}
