#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/ShapeCachePressurePolicy.hpp>
#include <limits>

using Poseidon::Streaming::ShapeCachePressurePolicy;

TEST_CASE("Shape cache pressure needs eight consecutive high samples and drops one", "[object-stream-pressure]")
{
    ShapeCachePressurePolicy policy;
    for (int i = 0; i < 7; ++i)
    {
        CHECK_FALSE(policy.Observe(true, 1001, 1000));
        CHECK(policy.DropCount(256) == 0);
    }
    CHECK(policy.Observe(true, 1001, 1000));
    CHECK(policy.DropCount(256) == 1);
    CHECK(policy.DropCount(1) == 1);
    CHECK(policy.DropCount(0) == 0);
    CHECK(policy.Observe(true, 1001, 1000)); // saturation never creates a larger batch
    CHECK(policy.DropCount(256) == 1);
}

TEST_CASE("Shape cache pressure uses a strict low mark and resets consecutive dwell", "[object-stream-pressure]")
{
    ShapeCachePressurePolicy policy;
    for (int i = 0; i < 8; ++i) policy.Observe(true, 1001, 1000);
    CHECK(policy.Active());
    for (int i = 0; i < 7; ++i) CHECK(policy.Observe(true, 849, 1000));
    CHECK(policy.Observe(true, 850, 1000)); // equality interrupts the low streak
    for (int i = 0; i < 7; ++i) CHECK(policy.Observe(true, 849, 1000));
    CHECK_FALSE(policy.Observe(true, 849, 1000));
    for (int i = 0; i < 7; ++i) CHECK_FALSE(policy.Observe(true, 1001, 1000));
    CHECK_FALSE(policy.Observe(true, 1000, 1000)); // equality interrupts the high streak
    for (int i = 0; i < 8; ++i) policy.Observe(true, 1001, 1000);
    CHECK(policy.Active());
}

TEST_CASE("Unavailable memory facts and world reset revoke cache shedding", "[object-stream-pressure]")
{
    ShapeCachePressurePolicy policy;
    for (int i = 0; i < 8; ++i) policy.Observe(true, 1001, 1000);
    CHECK_FALSE(policy.Observe(false, 1001, 1000));
    CHECK_FALSE(policy.Active());
    for (int i = 0; i < 8; ++i) policy.Observe(true, 1001, 1000);
    CHECK_FALSE(policy.Observe(true, 1001, 0));
    for (int i = 0; i < 8; ++i) policy.Observe(true, 1001, 1000);
    policy.Reset();
    CHECK_FALSE(policy.Active());
}

TEST_CASE("Shape cache pressure threshold math remains bounded at uint64 limits", "[object-stream-pressure]")
{
    ShapeCachePressurePolicy policy;
    const auto max = std::numeric_limits<uint64_t>::max();
    for (int i = 0; i < 8; ++i) policy.Observe(true, max, max - 1);
    CHECK(policy.Active());
    for (int i = 0; i < 8; ++i) policy.Observe(true, 0, max);
    CHECK_FALSE(policy.Active());
    ShapeCachePressurePolicy tiny;
    for (int i = 0; i < 8; ++i) tiny.Observe(true, 2, 1);
    CHECK(tiny.Active());
    for (int i = 0; i < 8; ++i) tiny.Observe(true, 0, 1);
    CHECK_FALSE(tiny.Active());
}
