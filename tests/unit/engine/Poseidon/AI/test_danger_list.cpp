// AI-XRAY-004: the danger list's priority rule (X-Ray CDangerManager: weight x 10 + age in ms, LOWEST wins) and
// the Lost Alpha ranges and memory it carries. Easy to get backwards, so pinned here.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/AI/AIDangerList.hpp>

using namespace Poseidon;
using DangerList::Priority;
using DangerList::Type;

TEST_CASE("a grenade outranks a ricochet of the same age", "[danger]")
{
    CHECK(Priority(Type::Grenade, 0) < Priority(Type::Ricochet, 0));
    CHECK(Priority(Type::Gunfire, 0) < Priority(Type::SquadmateDied, 0));
}

TEST_CASE("of two dangers of one kind the newer wins", "[danger]")
{
    CHECK(Priority(Type::Hit, 100) < Priority(Type::Hit, 5000));
}

TEST_CASE("age catches up with weight", "[danger]")
{
    // a grenade 20 s old (10000 + 20000) ranks with a fresh ricochet (30000)
    CHECK(Priority(Type::Grenade, 20000) == Priority(Type::Ricochet, 0));
    CHECK(Priority(Type::Grenade, 19999) < Priority(Type::Ricochet, 0));
}

TEST_CASE("ranges and memory follow Lost Alpha", "[danger]")
{
    CHECK(DangerList::Range(Type::Grenade) == 15);
    CHECK(DangerList::Memory(Type::Grenade) == 20);
    CHECK(DangerList::Range(Type::Corpse) == 20);
    CHECK(DangerList::Memory(Type::Corpse) == 10);
    CHECK(DangerList::Range(Type::Ricochet) == 10);
    CHECK(DangerList::Memory(Type::Hit) == 60);
}
