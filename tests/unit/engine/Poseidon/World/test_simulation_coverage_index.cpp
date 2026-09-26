#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/SimulationCoverageIndex.hpp>

using namespace Poseidon::Streaming;

TEST_CASE("Engine broadphase evidence covers animated guards and distant building origins", "[streaming-coverage]")
{
    const auto building = BuildEngineBroadphaseEnvelope({500, 20, 0}, 2, 150);
    REQUIRE(building);
    REQUIRE(building->min[0] < -100);
    REQUIRE(building->max[0] > 1100);
    SimulationCoverageIndex index;
    REQUIRE(index.Reset(1, 1, 1) == CoverageStatus::Ready);
    REQUIRE(index.Publish(1, 0, building, true));
    const auto hit = index.Lookup(1, {0, 20, -10}, {0, 20, 10}, 0, 1, 1);
    REQUIRE(hit.status == CoverageStatus::Ready);
    REQUIRE(hit.groups == std::vector<uint32_t>{0});
    // Radius must cover a static parent's doubled animation guard on every axis.
    REQUIRE(building->min[1] < -580);
    REQUIRE(building->max[2] > 600);
}

TEST_CASE("Uncertified groups block globally while certified disjoint groups permit progress", "[streaming-coverage]")
{
    SimulationCoverageIndex index;
    index.Reset(9, 2, 2);
    REQUIRE(index.Publish(9, 0, BuildEngineBroadphaseEnvelope({0, 0, 0}, 1, 3), true));
    auto lookup = [&] { return index.Lookup(9, {-1, 0, 0}, {1, 0, 0}, 0, 2, 2); };
    REQUIRE(lookup().status == CoverageStatus::Unknown);
    REQUIRE(lookup().groups.empty());
    // This certificate covers every placement in group 1, not just its origins.
    REQUIRE(index.Publish(9, 1, BuildEngineBroadphaseEnvelope({1000, 0, 0}, 1, 10), false));
    REQUIRE(lookup().status == CoverageStatus::Ready);
    REQUIRE(lookup().groups == std::vector<uint32_t>{0});
    const auto pending = index.Lookup(9, {990, 0, 0}, {1010, 0, 0}, 0, 2, 2);
    REQUIRE(pending.status == CoverageStatus::Unknown);
    REQUIRE(pending.groups.empty());
    REQUIRE(index.Publish(9, 1, BuildEngineBroadphaseEnvelope({1000, 0, 0}, 1, 10), true));
    REQUIRE(index.Lookup(9, {990, 0, 0}, {1010, 0, 0}, 0, 2, 2).groups == std::vector<uint32_t>{1});
}

TEST_CASE("Coverage query includes swept radius boundaries and reversed segments", "[streaming-coverage]")
{
    SimulationCoverageIndex index;
    index.Reset(1, 1, 1);
    REQUIRE(index.Publish(1, 0, CoverageEnvelope{{10, 10, 10}, {20, 20, 20}}, true));
    const auto forward = index.Lookup(1, {0, 9, 10}, {30, 9, 10}, 1, 1, 1);
    const auto reverse = index.Lookup(1, {30, 9, 10}, {0, 9, 10}, 1, 1, 1);
    REQUIRE(forward.status == CoverageStatus::Ready);
    REQUIRE(forward.groups == std::vector<uint32_t>{0});
    REQUIRE(reverse.groups == forward.groups);
    REQUIRE(index.Lookup(1, {0, 8, 10}, {30, 8, 10}, 1, 1, 0).groups.empty());
}

TEST_CASE("Coverage capacity refusal never supplies a partial Ready answer", "[streaming-coverage]")
{
    SimulationCoverageIndex index;
    REQUIRE(index.Reset(2, 2, 1) == CoverageStatus::CapacityExceeded);
    REQUIRE(index.Lookup(2, {}, {}, 0, 2, 2).status == CoverageStatus::CapacityExceeded);
    index.Reset(3, 2, 2);
    for (uint32_t i = 0; i < 2; ++i)
        REQUIRE(index.Publish(3, i, BuildEngineBroadphaseEnvelope({}, 1, 1), true));
    const auto scan = index.Lookup(3, {}, {}, 0, 1, 2);
    REQUIRE(scan.status == CoverageStatus::CapacityExceeded);
    REQUIRE(scan.groups.empty());
    const auto matches = index.Lookup(3, {}, {}, 0, 2, 1);
    REQUIRE(matches.status == CoverageStatus::CapacityExceeded);
    REQUIRE(matches.groups.empty());
}

TEST_CASE("Coverage updates coalesce and cancelled generations cannot republish stale data", "[streaming-coverage]")
{
    SimulationCoverageIndex index;
    index.Reset(1, 1, 1);
    const auto near = BuildEngineBroadphaseEnvelope({}, 1, 1);
    REQUIRE(index.Publish(1, 0, near, true));
    REQUIRE(index.Publish(1, 0, near, true));
    REQUIRE(index.Lookup(1, {}, {}, 0, 1, 1).groups.size() == 1);
    index.Reset(2, 1, 1);
    REQUIRE_FALSE(index.Publish(1, 0, near, true));
    REQUIRE(index.Lookup(1, {}, {}, 0, 1, 1).status == CoverageStatus::Invalid);
    REQUIRE(index.Lookup(2, {}, {}, 0, 1, 1).status == CoverageStatus::Unknown);
    index.Cancel(1); // stale cancellation cannot cancel the new world
    REQUIRE(index.Publish(2, 0, near, true));
    REQUIRE(index.Reset(1, 1, 1) == CoverageStatus::Invalid);
    REQUIRE(index.Lookup(2, {}, {}, 0, 1, 1).status == CoverageStatus::Ready);
    index.Cancel(2);
    REQUIRE_FALSE(index.Publish(2, 0, near, true));
    REQUIRE(index.Lookup(2, {}, {}, 0, 1, 1).status == CoverageStatus::Invalid);
    REQUIRE(index.Reset(2, 1, 1) == CoverageStatus::Invalid);
    REQUIRE_FALSE(index.Publish(2, 0, near, true));
    REQUIRE(index.Reset(3, 1, 1) == CoverageStatus::Ready);
    REQUIRE(index.Lookup(3, {}, {}, 0, 1, 1).status == CoverageStatus::Unknown);
}

TEST_CASE("Invalid numerical evidence poisons previous certificates and refuses invalid queries", "[streaming-coverage]")
{
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(BuildEngineBroadphaseEnvelope({nan, 0, 0}, 1, 1));
    REQUIRE_FALSE(BuildEngineBroadphaseEnvelope({}, 0, 1));
    REQUIRE_FALSE(BuildEngineBroadphaseEnvelope({}, -1, 1));
    REQUIRE_FALSE(BuildEngineBroadphaseEnvelope({}, 1, -1));
    REQUIRE_FALSE(BuildEngineBroadphaseEnvelope({}, 1, inf));
    REQUIRE_FALSE(BuildEngineBroadphaseEnvelope({}, std::numeric_limits<double>::max(), 1));
    REQUIRE_FALSE(BuildEngineBroadphaseEnvelope({1e30, 0, 0}, 1, 1));
    REQUIRE_FALSE(BuildEngineBroadphaseEnvelope({}, 1, 1e30));
    SimulationCoverageIndex index;
    index.Reset(3, 1, 1);
    REQUIRE(index.Publish(3, 0, BuildEngineBroadphaseEnvelope({}, 1, 1), true));
    REQUIRE_FALSE(index.Publish(3, 0, CoverageEnvelope{{0, 0, 0}, {nan, 1, 1}}, true));
    REQUIRE(index.Lookup(3, {}, {}, 0, 1, 1).status == CoverageStatus::Unknown);
    REQUIRE(index.Lookup(3, {inf, 0, 0}, {}, 0, 1, 1).status == CoverageStatus::Invalid);
    REQUIRE(index.Lookup(3, {}, {}, -1, 1, 1).status == CoverageStatus::Invalid);
    REQUIRE(index.Lookup(3, {1e30, 0, 0}, {}, 0, 1, 1).status == CoverageStatus::Invalid);
    REQUIRE(index.Lookup(3, {}, {}, 1e30, 1, 1).status == CoverageStatus::Invalid);
    REQUIRE(index.Reset(0, 0, 0) == CoverageStatus::Invalid);
}
