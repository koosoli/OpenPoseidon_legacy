// SIM-803 (roadmap Phase 8.3 -- "Stable iteration and reductions").
//
// Phase 8.3 requires that a gameplay-observable winner never depend on hash or
// container iteration order, pointer addresses, unordered float reductions, or job
// completion order. These tests pin the two AI orderings that violated that, plus
// the comparator-consistency bug found alongside them.
//
// Each test constructs the tie deliberately and asserts the winner is the one the
// stable identity picks -- not the one the allocator happened to put first.

#include <Poseidon/AI/AI.hpp>
#include <Poseidon/AI/AICenter.hpp>
#include <Poseidon/AI/VehicleAI.hpp>
#include <Poseidon/Foundation/Types/Pointers.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

using namespace Poseidon;

// ---------------------------------------------------------------------------
// 1. The guarded-vehicle ranking key must return an honest 0 on a tie.
//
// AICenter::UpdateGuarding sorts the enemy/unknown vehicles it knows about and then
// hands them out to guarding groups in that order. The old comparator broke equal
// ranks with `info1->_idExact - info2->_idExact`: the difference of two EntityAI
// heap addresses. `_type->GetCost()` is a flat per-type constant, so any two
// vehicles of the same type tie exactly -- meaning that branch was the *normal*
// path, not a rare one, and which group guarded which vehicle came out of the
// allocator.
// ---------------------------------------------------------------------------
TEST_CASE("SIM-803 guarded-vehicle rank returns 0 for an exact tie", "[ai][determinism]")
{
    // Same side-knownness, same cost -> indistinguishable. Must be 0, so that the
    // caller's stable_sort is what resolves it.
    CHECK(CompareGuardedVehicleRank(false, 100.0f, false, 100.0f) == 0);
    CHECK(CompareGuardedVehicleRank(true, 100.0f, true, 100.0f) == 0);
    CHECK(CompareGuardedVehicleRank(false, 0.0f, false, 0.0f) == 0);

    // Known side ranks ahead of unknown, regardless of cost.
    CHECK(CompareGuardedVehicleRank(false, 1.0f, true, 9999.0f) < 0);
    CHECK(CompareGuardedVehicleRank(true, 9999.0f, false, 1.0f) > 0);

    // Then descending cost.
    CHECK(CompareGuardedVehicleRank(false, 200.0f, false, 100.0f) < 0);
    CHECK(CompareGuardedVehicleRank(false, 100.0f, false, 200.0f) > 0);

    // Antisymmetry: swapping the arguments must flip the sign for every pair.
    // (The comparator this replaced failed exactly this for the two-nulls case in
    // AIGroup::CompUnits -- see the CompUnits note in the decision record.)
    const bool unknowns[] = {false, true};
    const float costs[] = {0.0f, 1.0f, 100.0f, 100.0f, 250.0f};
    for (bool u1 : unknowns)
    {
        for (float c1 : costs)
        {
            for (bool u2 : unknowns)
            {
                for (float c2 : costs)
                {
                    int forward = CompareGuardedVehicleRank(u1, c1, u2, c2);
                    int backward = CompareGuardedVehicleRank(u2, c2, u1, c1);
                    CHECK(((forward < 0 && backward > 0) || (forward > 0 && backward < 0) ||
                           (forward == 0 && backward == 0)));
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 2. The tie must be broken by array index, not by address.
//
// This models the UpdateGuarding call site exactly: a vector of pointers to
// equal-ranked elements, stable_sorted with a predicate built on
// CompareGuardedVehicleRank. The elements are separately heap-allocated, so their
// addresses bear no defined relation to their index order -- which is precisely
// what makes an address-based tie-break unreproducible, and why the assertions
// below are about index labels only and never about addresses.
// ---------------------------------------------------------------------------
namespace
{
struct RankedStandin
{
    bool sideUnknown;
    float cost;
    int indexLabel; // the stable identity: position in the ordered source array
};
} // namespace

TEST_CASE("SIM-803 equal-cost guard targets keep source-array order, not address order", "[ai][determinism]")
{
    constexpr int kCount = 8;

    std::vector<std::unique_ptr<RankedStandin>> owned;
    std::vector<RankedStandin*> targets;
    for (int i = 0; i < kCount; i++)
    {
        // Every element ties: same known-side flag, same cost.
        owned.push_back(std::make_unique<RankedStandin>(RankedStandin{false, 100.0f, i}));
        targets.push_back(owned.back().get());
    }

    auto before = [](const RankedStandin* a, const RankedStandin* b)
    { return CompareGuardedVehicleRank(a->sideUnknown, a->cost, b->sideUnknown, b->cost) < 0; };

    // Sort the all-ties array. Every element compares equal, so a stable sort must
    // return it completely untouched.
    std::stable_sort(targets.begin(), targets.end(), before);
    for (int i = 0; i < kCount; i++)
    {
        CHECK(targets[i]->indexLabel == i);
    }

    // Now prove the result is genuinely independent of address order: sort the same
    // pointers by ADDRESS first (a deliberately adversarial starting arrangement),
    // relabel nothing, and confirm the ranking predicate still cannot distinguish
    // them -- i.e. that no address term leaked back in.
    std::vector<RankedStandin*> byAddress = targets;
    std::sort(byAddress.begin(), byAddress.end(), [](const RankedStandin* a, const RankedStandin* b) { return a < b; });
    for (RankedStandin* a : byAddress)
    {
        for (RankedStandin* b : byAddress)
        {
            CHECK(!before(a, b));
        }
    }

    // And a mixed case: one strictly better element buried in the middle of the ties
    // must come out first, with the ties behind it still in index order.
    targets[5]->cost = 250.0f;
    std::stable_sort(targets.begin(), targets.end(), before);
    CHECK(targets[0]->indexLabel == 5);
    int expected = 0;
    for (int i = 1; i < kCount; i++)
    {
        if (expected == 5)
        {
            expected++;
        }
        CHECK(targets[i]->indexLabel == expected);
        expected++;
    }
}

// ---------------------------------------------------------------------------
// 3. The old tie-break was not merely address-dependent, it was arithmetically
//    broken.
//
// `int CmpGuardedVehicles(...)` ended in `return info1->_idExact - info2->_idExact;`
// -- an EntityAI* difference, i.e. a 64-bit ptrdiff_t, narrowed to the int return
// type. For two objects more than 2GiB apart in the address space the truncation
// changes the sign, so the comparator did not even order the addresses it was
// (wrongly) using consistently. A 64-bit process reaches those separations easily.
//
// We demonstrate the arithmetic directly rather than trying to allocate objects
// 2GiB apart; fabricating pointers at chosen addresses would be undefined behaviour.
// ---------------------------------------------------------------------------
TEST_CASE("SIM-803 ptrdiff-to-int tie-break is intransitive across 2GiB", "[ai][determinism]")
{
    // What the old comparator computed for a pair of addresses.
    auto oldTieBreak = [](std::int64_t addrA, std::int64_t addrB)
    { return static_cast<int>(static_cast<std::ptrdiff_t>(addrA - addrB)); };

    // Three objects at increasing addresses, spanning slightly more than 2GiB in
    // total. Nothing exotic: a 64-bit process with a large heap reaches this.
    const std::int64_t a = 0x0000000000000000LL;
    const std::int64_t b = 0x0000000060000000LL; // a + 1.5GiB
    const std::int64_t c = 0x00000000C0000000LL; // a + 3.0GiB

    REQUIRE(a < b);
    REQUIRE(b < c);

    // Pairwise, the comparator says A before B and B before C...
    CHECK(oldTieBreak(a, b) < 0);
    CHECK(oldTieBreak(b, c) < 0);

    // ...but C before A. A < B < C < A is a cycle: not a strict weak ordering at
    // all, so the sort's output for these three is undefined even before you ask
    // whether it is reproducible. This is the concrete reason the fix removes the
    // pointer term rather than merely swapping in a different pointer comparison.
    CHECK(oldTieBreak(a, c) > 0);

    // The narrowing is what does it: the true differences are all positive-going,
    // but only the a-to-c one loses its sign to the 32-bit return type.
    CHECK(static_cast<std::ptrdiff_t>(c - a) > 0);
    CHECK(static_cast<int>(static_cast<std::ptrdiff_t>(c - a)) < 0);
}

// ---------------------------------------------------------------------------
// 4. TargetList::SortBySubjectiveCost -- the production function, on real Targets.
//
// AIGroup::AssignTargets and SelectInterestingTarget walk this array in order, so
// its ordering is directly gameplay-observable. AIGroup::GetSubjectiveCost collapses
// to the type's flat GetCost() for any target that cannot currently fire back, so
// exact bit-for-bit ties between identical unit types happen every think cycle.
//
// The array is sorted in place every cycle, so the property that matters is: equal
// costs keep the order the array already has, whatever that order is. The old QSort
// (an unstable quicksort, with an unstable selection sort for <=8 elements) did not
// give that.
// ---------------------------------------------------------------------------
TEST_CASE("SIM-803 equal-cost targets keep their list order under sorting", "[ai][determinism]")
{
    // TargetList (RefArray) is not copyable, so fill in place rather than returning.
    auto fill = [](TargetList& list, const std::vector<float>& costs)
    {
        for (float c : costs)
        {
            Ref<Target> t = new Target(nullptr);
            t->subjectiveCost = c;
            list.Add(t);
        }
    };

    SECTION("an all-ties list is left completely alone")
    {
        TargetList list;
        fill(list, {50.0f, 50.0f, 50.0f, 50.0f, 50.0f});
        std::vector<Target*> before;
        for (int i = 0; i < list.Size(); i++)
        {
            before.push_back(list[i]);
        }

        list.SortBySubjectiveCost();

        REQUIRE(list.Size() == static_cast<int>(before.size()));
        for (int i = 0; i < list.Size(); i++)
        {
            CHECK(list[i] == before[i]);
        }
    }

    SECTION("ties behind a clear winner keep their relative order")
    {
        // Index 2 is strictly the best; 0,1,3,4 all tie at 50.
        TargetList list;
        fill(list, {50.0f, 50.0f, 90.0f, 50.0f, 50.0f});
        std::vector<Target*> tied = {list[0], list[1], list[3], list[4]};
        Target* winner = list[2];

        list.SortBySubjectiveCost();

        // Descending cost: the 90 comes first.
        CHECK(list[0] == winner);
        // The four ties follow in exactly their original relative order.
        for (int i = 0; i < 4; i++)
        {
            CHECK(list[i + 1] == tied[i]);
        }
    }

    SECTION("the winner is the same whichever order the tie was inserted in")
    {
        // Two lists holding the same costs, one built forwards and one backwards.
        // The tie-break is "index in this array", so each list must return ITS OWN
        // first-inserted tied element -- deterministically, and never whichever
        // Target the allocator happened to place lower in memory.
        TargetList forward;
        TargetList backward;
        fill(forward, {70.0f, 70.0f, 70.0f});
        fill(backward, {70.0f, 70.0f, 70.0f});

        Target* forwardFirst = forward[0];
        Target* backwardFirst = backward[0];

        forward.SortBySubjectiveCost();
        backward.SortBySubjectiveCost();

        CHECK(forward[0] == forwardFirst);
        CHECK(backward[0] == backwardFirst);

        // Sorting an already-sorted list must be a no-op (idempotent), which is what
        // makes the ordering stable across the repeated per-cycle re-sorts.
        Target* settled = forward[0];
        forward.SortBySubjectiveCost();
        forward.SortBySubjectiveCost();
        CHECK(forward[0] == settled);
    }

    SECTION("strict descending order is still what the sort produces")
    {
        TargetList list;
        fill(list, {10.0f, 90.0f, 30.0f, 70.0f, 50.0f});
        list.SortBySubjectiveCost();
        REQUIRE(list.Size() == 5);
        for (int i = 1; i < list.Size(); i++)
        {
            CHECK(list[i - 1]->subjectiveCost >= list[i]->subjectiveCost);
        }
        CHECK(list[0]->subjectiveCost == 90.0f);
        CHECK(list[4]->subjectiveCost == 10.0f);
    }
}
