#include <catch2/catch_test_macros.hpp>
#include <Poseidon/Foundation/Containers/Array.hpp>
#include <Poseidon/World/Simulation/StableExtract.hpp>
using namespace Poseidon;
using namespace Poseidon::Foundation;
namespace
{
struct Item : RefCount
{
    int id;
    int& destroyed;
    Item(int value, int& count) : id(value), destroyed(count) {}
    ~Item() { ++destroyed; }
};
}
TEST_CASE("detail transfers retain ownership and stable ordering", "[sim-distribution]")
{
    int destroyed = 0;
    {
        RefArray<Item> source, even, odd;
        for (int i = 0; i < 20000; ++i) source.Add(new Item(i, destroyed));
        int calls = 0;
        StableExtract(source, [&](Item* item) {
            ++calls;
            if (item->id % 3 == 0) return false;
            (item->id % 2 ? odd : even).Add(item);
            return true;
        });
        CHECK(calls == 20000);
        CHECK(destroyed == 0);
        CHECK(source.Size() + even.Size() + odd.Size() == 20000);
        for (const auto* list : {&source, &even, &odd})
            for (int i = 1; i < list->Size(); ++i) REQUIRE((*list)[i-1]->id < (*list)[i]->id);
        StableExtract(source, [&](Item* item) { even.Add(item); return true; });
        CHECK(source.Size() == 0);
        CHECK(destroyed == 0);
        StableExtract(source, [](Item*) { FAIL("empty list called predicate"); return true; });
    }
    CHECK(destroyed == 20000);
}
TEST_CASE("unchanged detail groups retain exact item identity", "[sim-distribution]")
{
    int destroyed = 0;
    RefArray<Item> list;
    Ref<Item> item = new Item(7, destroyed);
    list.Add(item);
    StableExtract(list, [](Item*) { return false; });
    REQUIRE(list.Size() == 1);
    CHECK(list[0] == item);
    CHECK(destroyed == 0);
}
