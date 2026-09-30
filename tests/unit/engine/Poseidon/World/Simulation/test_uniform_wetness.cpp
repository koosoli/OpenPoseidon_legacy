#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <Poseidon/World/Entities/Infantry/UniformWetness.hpp>
#include <Poseidon/Graphics/Rendering/UniformClothAdmission.hpp>
#include <limits>

using Poseidon::AdvanceUniformWetness;
using Poseidon::UniformWettingDensity;
using namespace Poseidon::render;

TEST_CASE("Uniform exposure includes visible liquid rain without mistaking snow for rain", "[simulation][uniform-wetness]")
{
    REQUIRE(UniformWettingDensity(0, 1, false) == 1); // liquid particle override on a clear day
    REQUIRE(AdvanceUniformWetness(0, UniformWettingDensity(0, 1, false), false, 20) > 0.35f);
    REQUIRE(AdvanceUniformWetness(0, UniformWettingDensity(0, 1, false), true, 20) == 0);
    REQUIRE(UniformWettingDensity(0.4f, 1, false) == 1);
    REQUIRE(UniformWettingDensity(0.7f, 0.2f, false) == 0.7f);
    REQUIRE(UniformWettingDensity(0.6f, 0, false) == 0.6f); // particles off/legacy only
    REQUIRE(UniformWettingDensity(0, 0, false) == 0); // disabled override cannot create rain
    REQUIRE(UniformWettingDensity(0, 1, true) == 0); // snowflake density override is not wetting proof
    REQUIRE(UniformWettingDensity(0.6f, 1, true) == 0.6f); // actual weather rain is retained
    REQUIRE(UniformWettingDensity(-1, -1, false) == 0);
    REQUIRE(UniformWettingDensity(2, 0, true) == 1);
    REQUIRE(UniformWettingDensity(0, 2, false) == 1);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    REQUIRE(UniformWettingDensity(nan, 0.3f, false) == 0.3f);
    REQUIRE(UniformWettingDensity(0.3f, inf, false) == 0.3f);
    REQUIRE(UniformWettingDensity(-inf, nan, false) == 0);
    REQUIRE(UniformWettingDensity(nan, inf, true) == 0);
}

TEST_CASE("Uniform water retention follows actual simulation exposure", "[simulation][uniform-wetness]")
{
    REQUIRE(AdvanceUniformWetness(0, 1, false, 20) == Catch::Approx(1.0 - std::exp(-20.0 / 45.0)));
    REQUIRE(AdvanceUniformWetness(0, 1, false, 60) > 0.73f);
    REQUIRE(AdvanceUniformWetness(0, 1, true, 120) == 0.0f);
    REQUIRE(AdvanceUniformWetness(0.8f, 1, true, 120) == Catch::Approx(0.8 * std::exp(-120.0 / 600.0)));
    REQUIRE(AdvanceUniformWetness(0.8f, 0, false, 120) == AdvanceUniformWetness(0.8f, 1, true, 120));
    REQUIRE(AdvanceUniformWetness(0.3f, 1, false, 0) == 0.3f);
    REQUIRE(AdvanceUniformWetness(0.3f, 1, false, -1) == 0.3f);
}

TEST_CASE("Uniform wetness is independent of frame partition and handles exposure changes", "[simulation][uniform-wetness]")
{
    for (float rain : {0.0f, 0.25f, 1.0f})
    {
        float split = 0.3f;
        for (int i = 0; i < 720; ++i) split = AdvanceUniformWetness(split, rain, false, 1.0f / 6.0f);
        REQUIRE(split == Catch::Approx(AdvanceUniformWetness(0.3f, rain, false, 120)).margin(0.00001));
    }
    const float wet = AdvanceUniformWetness(0, 1, false, 120);
    const float roof = AdvanceUniformWetness(wet, 1, true, 120);
    REQUIRE(roof < wet);
    REQUIRE(roof > 0.0f); // a roof does not erase retained water instantly
    REQUIRE(AdvanceUniformWetness(roof, 1, false, 20) > roof);
    REQUIRE(AdvanceUniformWetness(0, 1, false, 100000) == 1.0f);
    REQUIRE(AdvanceUniformWetness(1, 0, false, 100000) < 0.000001f);
}

TEST_CASE("Uniform wetness rejects nonfinite time and bounds weather/history", "[simulation][uniform-wetness]")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    REQUIRE(AdvanceUniformWetness(0.4f, 1, false, nan) == 0.4f);
    REQUIRE(AdvanceUniformWetness(0.4f, 1, false, inf) == 0.4f);
    REQUIRE(AdvanceUniformWetness(nan, 0, false, 10) == 0);
    REQUIRE(AdvanceUniformWetness(0.4f, nan, false, 10) < 0.4f);
    REQUIRE(AdvanceUniformWetness(0, 9, false, 20) == AdvanceUniformWetness(0, 1, false, 20));
    REQUIRE(AdvanceUniformWetness(1, -9, false, 20) == AdvanceUniformWetness(1, 0, false, 20));
}

TEST_CASE("Only verified primary-body stock cloth materials admit wet shading", "[rendering][uniform-wetness]")
{
    REQUIRE(AdmitUniformCloth("data3d\\mc vojakw2.p3d", "merged\\00007mc_vojakw2.paa") == UniformCloth::WestAtlas);
    REQUIRE(AdmitUniformCloth("DATA3D/MC VOJAKE2.P3D", "MERGED/00008MC_VOJAKE2.PAA") == UniformCloth::EastAtlas);
    REQUIRE(AdmitUniformCloth("data3d\\mc vojakw2.p3d", "data\\xicht_a.paa") == UniformCloth::None);
    REQUIRE(AdmitUniformCloth("data3d\\mc vojakw2.p3d", "data\\civilista_bryle.paa") == UniformCloth::None);
    REQUIRE(AdmitUniformCloth("data3d\\ak_47.p3d", "merged\\00007mc_vojakw2.paa") == UniformCloth::None);
    REQUIRE(AdmitUniformCloth("mod\\mc vojakw2.p3d", "merged\\00007mc_vojakw2.paa") == UniformCloth::None);
    REQUIRE(AdmitUniformCloth("data3d\\mc pilote2.p3d", "data\\e_pilot_hrud_p.pac") == UniformCloth::Plain);
    REQUIRE(AdmitUniformCloth("data3d\\mc vojakw2.p3d", "mod\\00007mc_vojakw2.paa") == UniformCloth::None);
    REQUIRE(AdmitUniformCloth("", "") == UniformCloth::None);
    for (auto cloth : {UniformCloth::None, UniformCloth::Plain, UniformCloth::WestAtlas, UniformCloth::EastAtlas})
        REQUIRE(EncodeUniformWetness(cloth, 0) == 0);
    REQUIRE(EncodeUniformWetness(UniformCloth::None, 1) == 0);
    REQUIRE(EncodeUniformWetness(UniformCloth::Plain, 0.5f) == 0.5f);
    REQUIRE(EncodeUniformWetness(UniformCloth::WestAtlas, 0.5f) == 2.5f);
    REQUIRE(EncodeUniformWetness(UniformCloth::EastAtlas, 0.5f) == 4.5f);
    REQUIRE(EncodeUniformWetness(UniformCloth::WestAtlas, 2) == 3);
    REQUIRE(EncodeUniformWetness(UniformCloth::EastAtlas, std::numeric_limits<float>::quiet_NaN()) == 0);
}
