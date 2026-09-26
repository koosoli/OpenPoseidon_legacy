#include <catch2/catch_test_macros.hpp>
#include <Poseidon/World/Terrain/AuthoredObjectIds.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace
{
struct Placement
{
    int id;
    bool resident;
};
}

TEST_CASE("Unseen authored placements reserve the dynamic object ID namespace", "[streaming-ids]")
{
    const std::array placements{Placement{42, true}, Placement{83912800, false}, Placement{150, true}};
    int lastId = Poseidon::Streaming::AuthoredObjectIdFloor(true, placements);
    REQUIRE(lastId == 83912800);
    const int nextDynamicId = ++lastId;
    for (const auto& placement : placements)
        REQUIRE(nextDynamicId > placement.id);
    auto returned = placements;
    returned[1].resident = true;
    REQUIRE(Poseidon::Streaming::AuthoredObjectIdFloor(true, returned) == 83912800);
}

TEST_CASE("Authored ID floors ignore invalid IDs and preserve larger restored counters", "[streaming-ids]")
{
    const std::array placements{Placement{-20, false}, Placement{-1, false}, Placement{0, false}, Placement{99, false}};
    const int floor = Poseidon::Streaming::AuthoredObjectIdFloor(true, placements);
    REQUIRE(floor == 99);
    REQUIRE(std::max(42, floor) == 99); // Counter from an older resident-only save.
    REQUIRE(std::max(200, floor) == 200); // Already allocated dynamic IDs remain reserved.
    const std::array invalid{Placement{-10, false}, Placement{-1, true}};
    REQUIRE(Poseidon::Streaming::AuthoredObjectIdFloor(true, invalid) == -1);
    REQUIRE(Poseidon::Streaming::AuthoredObjectIdFloor(true, std::array<Placement, 0>{}) == -1);
}

TEST_CASE("Legacy ID rebuilding keeps its original resident-derived floor", "[streaming-ids]")
{
    const std::array placements{Placement{83912800, false}};
    REQUIRE(Poseidon::Streaming::AuthoredObjectIdFloor(false, placements) == -1);
    REQUIRE(Poseidon::Streaming::AuthoredObjectIdFloor(true,
        std::array{Placement{std::numeric_limits<int>::max(), false}}) == std::numeric_limits<int>::max());
}
