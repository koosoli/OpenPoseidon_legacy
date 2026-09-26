#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/SimulationDemand.hpp>

using namespace Poseidon::Streaming;

TEST_CASE("Simulation corridor plans include crossed cells and touching boundaries", "[streaming-demand]")
{
    auto horizontal = PlanSimulationDemand(5, 5, 35, 5, 0, 10, 4, 64);
    REQUIRE(horizontal.status == DemandStatus::Complete);
    REQUIRE(horizontal.cells == std::vector<uint32_t>{0, 1, 2, 3});
    auto vertical = PlanSimulationDemand(5, 5, 5, 35, 0, 10, 4, 64);
    REQUIRE(vertical.cells == std::vector<uint32_t>{0, 4, 8, 12});
    auto point = PlanSimulationDemand(10, 10, 10, 10, 0, 10, 4, 64);
    REQUIRE(point.cells == std::vector<uint32_t>{0, 1, 4, 5});
    auto diagonal = PlanSimulationDemand(5, 5, 35, 35, 0, 10, 4, 64);
    REQUIRE(diagonal.cells == std::vector<uint32_t>{0, 1, 4, 5, 6, 9, 10, 11, 14, 15});
}

TEST_CASE("Simulation demands conservatively account for placement extents", "[streaming-demand]")
{
    auto padded = PlanSimulationDemand(15, 15, 15, 15, 6, 10, 4, 64);
    REQUIRE(padded.status == DemandStatus::Complete);
    REQUIRE(padded.cells == std::vector<uint32_t>{0, 1, 2, 4, 5, 6, 8, 9, 10});
    auto edge = PlanSimulationDemand(-20, 5, 5, 5, 0, 10, 4, 64);
    REQUIRE(edge.cells == std::vector<uint32_t>{0});
    auto outside = PlanSimulationDemand(-20, -20, -10, -10, 0, 10, 4, 64);
    REQUIRE(outside.status == DemandStatus::Complete);
    REQUIRE(outside.cells.empty());
    auto reachesEdge = PlanSimulationDemand(-5, 5, -5, 5, 6, 10, 4, 64);
    REQUIRE(reachesEdge.cells == std::vector<uint32_t>{0, 4});
}

TEST_CASE("Refused simulation demands never publish partial clearance", "[streaming-demand]")
{
    auto full = PlanSimulationDemand(5, 5, 35, 35, 0, 10, 4, 9);
    REQUIRE(full.status == DemandStatus::CapacityExceeded);
    REQUIRE(full.cells.empty());
    auto zero = PlanSimulationDemand(5, 5, 5, 5, 0, 10, 4, 0);
    REQUIRE(zero.status == DemandStatus::CapacityExceeded);
    REQUIRE(zero.cells.empty());
    auto invalid = PlanSimulationDemand(std::numeric_limits<double>::infinity(), 5, 5, 5, 0, 10, 4, 64);
    REQUIRE(invalid.status == DemandStatus::Invalid);
    REQUIRE(invalid.cells.empty());
    REQUIRE(PlanSimulationDemand(0, 0, 1, 1, -1, 10, 4, 64).status == DemandStatus::Invalid);
    REQUIRE(PlanSimulationDemand(0, 0, 1, 1, 0, 0, 4, 64).status == DemandStatus::Invalid);
    REQUIRE(PlanSimulationDemand(0, 0, 1, 1, 0, 10, 65536, 64).status == DemandStatus::Invalid);
}

TEST_CASE("Long diagonal simulation corridors avoid quadratic cell demand", "[streaming-demand]")
{
    auto longRay = PlanSimulationDemand(5, 5, 995, 995, 0, 10, 100, 400);
    REQUIRE(longRay.status == DemandStatus::Complete);
    REQUIRE(longRay.cells.size() == 298);
    REQUIRE(std::is_sorted(longRay.cells.begin(), longRay.cells.end()));
    REQUIRE(std::adjacent_find(longRay.cells.begin(), longRay.cells.end()) == longRay.cells.end());
    auto reverse = PlanSimulationDemand(995, 995, 5, 5, 0, 10, 100, 400);
    REQUIRE(reverse.cells == longRay.cells);
}

TEST_CASE("Simulation demand covers an independent exhaustive slab oracle", "[streaming-demand]")
{
    // Integer coordinates give exact crossing fixtures, including corners and out-of-world paths.
    // Oracle checks every cell independently rather than repeating the planner's row traversal.
    for (int fixture = 0; fixture < 120; ++fixture)
    {
        const double ax = (fixture * 7 % 81) - 20, az = (fixture * 11 % 81) - 20;
        const double bx = (fixture * 17 % 81) - 20, bz = (fixture * 23 % 81) - 20;
        const double padding = fixture % 8;
        const auto plan = PlanSimulationDemand(ax, az, bx, bz, padding, 10, 4, 64);
        REQUIRE(plan.status == DemandStatus::Complete);
        for (uint32_t z = 0; z < 4; ++z)
            for (uint32_t x = 0; x < 4; ++x)
            {
                double t0 = 0, t1 = 1;
                const double origins[] = {ax, az}, deltas[] = {bx - ax, bz - az};
                const double lo[] = {x * 10.0 - padding, z * 10.0 - padding};
                const double hi[] = {(x + 1) * 10.0 + padding, (z + 1) * 10.0 + padding};
                bool intersects = true;
                for (int axis = 0; axis < 2; ++axis)
                {
                    if (deltas[axis] == 0)
                        intersects &= origins[axis] >= lo[axis] && origins[axis] <= hi[axis];
                    else
                    {
                        const double u = (lo[axis] - origins[axis]) / deltas[axis];
                        const double v = (hi[axis] - origins[axis]) / deltas[axis];
                        t0 = std::max(t0, std::min(u, v));
                        t1 = std::min(t1, std::max(u, v));
                    }
                }
                if (intersects && t0 <= t1)
                    REQUIRE(std::binary_search(plan.cells.begin(), plan.cells.end(), z * 4 + x));
            }
        const auto reversed = PlanSimulationDemand(bx, bz, ax, az, padding, 10, 4, 64);
        REQUIRE(reversed.cells == plan.cells);
    }
}
