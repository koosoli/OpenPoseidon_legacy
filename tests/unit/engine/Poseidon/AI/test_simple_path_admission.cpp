#include <catch2/catch_test_macros.hpp>
#include <Poseidon/AI/Path/SimplePathAdmission.hpp>
#include <limits>
#include <Poseidon/World/Scene/BenchmarkLodFix.hpp>

TEST_CASE("Simple paths refuse malformed endpoints before operational map conversion", "[ai][path][upstream]")
{
    const std::array<float, 3> ordinary{5805.63f, 46.35f, 3597.97f};
    CHECK(Poseidon::SimplePathEndpointsAdmitted(ordinary, ordinary, .25f));
    CHECK(Poseidon::SimplePathEndpointsAdmitted({-4, 0, -4}, {4, 0, 4}, .25f));
    for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()})
    for (int axis = 0; axis < 3; ++axis)
    {
        auto malformed = ordinary;
        malformed[axis] = invalid;
        CHECK_FALSE(Poseidon::SimplePathEndpointsAdmitted(malformed, ordinary, .25f));
        CHECK_FALSE(Poseidon::SimplePathEndpointsAdmitted(ordinary, malformed, .25f));
    }
    CHECK_FALSE(Poseidon::SimplePathEndpointsAdmitted({1e30f, 0, 0}, {1e30f, 0, 0}, .25f));
    CHECK_FALSE(Poseidon::SimplePathEndpointsAdmitted({-1e30f, 0, 0}, {0, 0, -1e30f}, .25f));
    CHECK_FALSE(Poseidon::SimplePathEndpointsAdmitted(ordinary, ordinary, 0));
    CHECK_FALSE(Poseidon::SimplePathEndpointsAdmitted(ordinary, ordinary, std::numeric_limits<float>::infinity()));
}

TEST_CASE("Benchmark LOD freeze preserves unset adaptation and excludes nonfinite density", "[graphics][lod][upstream]")
{
    CHECK(Poseidon::ParseBenchmarkLodFix(nullptr) == 0);
    CHECK(Poseidon::ParseBenchmarkLodFix("0.125") == .125f);
    for (const char* value : {"true", "", "0", "-1", "nan", "inf", "1e100"})
        CHECK(Poseidon::ParseBenchmarkLodFix(value) == -1);
}
