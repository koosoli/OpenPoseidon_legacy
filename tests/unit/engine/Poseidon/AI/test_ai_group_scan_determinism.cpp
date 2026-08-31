// SIM-813 -- roadmap Phase 8, and the top-ranked finding of SIM-812's mutation audit:
// "AICenter::UpdateGroup decides which groups think this tick from a wall-clock budget.
// How many groups think in a tick is a function of real CPU time, so the AI stage is not
// deterministic today, before any parallelism."
//
// The property under test is exactly one sentence: THE SET AND ORDER OF GROUPS THAT THINK
// IN A TICK IS A FUNCTION OF SIMULATION STATE ONLY, NEVER OF HOW FAST THIS MACHINE RAN.
//
// How that is tested, and what the test is honest about:
//
//   * `AICenter::UpdateGroup` cannot be instantiated in a unit test -- it needs a World, a
//     Glob and live `AIGroup`s. What CAN be lifted out is the loop's stopping decision,
//     which is 100% of the defect, and `GroupScanBudget` is that decision as SHIPPED code:
//     `UpdateGroup` calls this class, not a copy of it.
//
//   * `WalkModel` below reproduces `UpdateGroup`'s walk structure literally -- oldest-first
//     selection by `age = centerTime - groupTime`, strict `maxAge < age`, stamp on visit,
//     stop when one group reports work -- with the stopping policy as a parameter. It IS a
//     model, stated as one; the shipped budget object is the thing it drives.
//
//   * The two runs differ ONLY in a simulated per-group cost. That cost is what a real
//     machine's speed varies, and under the wall-clock policy it changes the answer.
//
// Every equivalence case has an ablation beside it, per SIM-806/808/811/812. The ablations
// are recorded in the decision document with the tick and field they named.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/AI/AIGroupScanBudget.hpp>
#include <Poseidon/Core/StateTimeline.hpp>

#include <cstdint>
#include <vector>

using Poseidon::Determinism::CompareTimelines;
using Poseidon::Determinism::DivergenceKind;
using Poseidon::Determinism::GroupScanBudget;
using Poseidon::Determinism::StateTimeline;

namespace
{

constexpr int kGroups = 16;
constexpr int kTicks = 96;

// ---------------------------------------------------------------------------
// A model of AICenter::UpdateGroup's walk.
//
// Mirrors AICenterImplPreview.cpp:486-522 field for field:
//
//     for (int i = 0; i < GroupsPerCycle;)          // GroupsPerCycle is 1
//     {
//         pick the group maximising (centerTime - group.time), strictly
//         if none -> centerTime = simTime; break
//         if (group.Think()) i++
//         group.time = centerTime
//         <STOPPING POLICY HERE>
//     }
//
// `Think()` returning false is what makes the walk visit more than one group, and it is
// common: a non-local group, a group whose subgroups all reported no path work, a group
// that destroyed itself. That is why the budget was ever load-bearing.
// ---------------------------------------------------------------------------

struct Group
{
    float time = 0.0f; // _lastUpdateTime
    int cost = 1;      // simulated CPU cost of one Think(), in arbitrary units
};

/// What `AIGroup::Think()` returned -- a function of SIMULATION STATE, so it depends on the
/// tick as well as the group. Making it depend on the tick is not decoration: a model whose
/// answers repeated every cycle would produce a handful of distinct tick hashes and would
/// agree with a wrong implementation on most of its ticks. SIM-812 shipped that mistake and
/// caught it on the anti-vacuity check.
using ThinkFn = bool (*)(int groupIndex, int tick);

/// FIRST VERSION OF THIS WAS `((groupIndex + tick) % 5) == 4` AND IT WAS WRONG.
/// It made the walk periodic: 96 ticks produced 8 distinct tick hashes, so a run would
/// have agreed with a broken implementation on 88 of them. The anti-vacuity check below
/// caught it, which is the second time in two sessions that check has earned its keep
/// (SIM-812 §4). A well-mixed function of the same two state variables is not periodic
/// over the run and is no less deterministic.
bool ThinksByState(int groupIndex, int tick)
{
    const std::uint32_t mixed =
        (static_cast<std::uint32_t>(groupIndex) * 2654435761u) ^ (static_cast<std::uint32_t>(tick) * 2246822519u);
    return ((mixed >> 13) % 6u) == 0u;
}

/// The walk's worst case, and not a hypothetical one: every group non-local, or every
/// group's subgroups reporting no path work, makes `AIGroup::Think()` return false for all
/// of them and the walk visits the entire list.
bool ThinksNever(int /*groupIndex*/, int /*tick*/)
{
    return false;
}

struct WalkResult
{
    std::vector<int> visited; // group indices, in visit order
    int spent = 0;
};

/// Where in the loop a stopping decision is being asked for. Both phases exist because
/// the two policies check in different places -- the shipped budget on loop ENTRY, the old
/// wall clock at the BOTTOM, after the stamp, exactly as the source did.
enum class Phase
{
    Entry,
    Bottom,
};

/// A stopping policy. Returns true when the walk must stop.
/// `spent` is accumulated simulated cost -- the stand-in for the wall clock.
///
/// THE POLICY IS THE ONLY THING `Walk` CONSULTS. An earlier version of this file special-
/// cased the two policies inside `Walk` and called `budget.Exhausted()` directly for the
/// deterministic one; ablating the policy then changed nothing, because `Walk` never called
/// it. The ablation reported "all tests passed" and would have shipped a test that could
/// not fail. Routing BOTH policies through the same call is what makes the ablation real.
using StopPolicy = bool (*)(GroupScanBudget&, int spent, Phase phase);

constexpr int kWallClockBudget = 40;

/// SHIPPED behaviour: the budget object decides, on loop entry, and never sees `spent`.
bool StopDeterministic(GroupScanBudget& budget, int /*spent*/, Phase phase)
{
    return phase == Phase::Entry && budget.Exhausted();
}

/// ABLATION ONLY: the pre-SIM-813 behaviour. `spent` stands in for
/// `CompareSectionTimeGE(sectionTime, 0.020f)`, checked where the source checked it.
bool StopWallClock(GroupScanBudget& /*budget*/, int spent, Phase phase)
{
    return phase == Phase::Bottom && spent >= kWallClockBudget;
}

/// One `UpdateGroup` call. `costScale` multiplies every group's Think() cost -- it is the
/// machine, and nothing else in this function may consult it.
WalkResult Walk(std::vector<Group>& groups, float& centerTime, float simTime, int tick, int costScale, ThinkFn thinks,
                StopPolicy stop)
{
    WalkResult out;
    GroupScanBudget budget(static_cast<int>(groups.size()));

    for (int i = 0; i < 1;)
    {
        if (stop(budget, out.spent, Phase::Entry))
        {
            break;
        }

        float maxAge = 0.0f;
        int maxIdx = -1;
        for (int j = 0; j < static_cast<int>(groups.size()); j++)
        {
            const float age = centerTime - groups[j].time;
            if (maxAge < age)
            {
                maxAge = age;
                maxIdx = j;
            }
        }
        if (maxIdx < 0)
        {
            centerTime = simTime;
            break;
        }

        out.visited.push_back(maxIdx);
        out.spent += groups[maxIdx].cost * costScale;
        if (thinks(maxIdx, tick))
        {
            i++;
        }
        groups[maxIdx].time = centerTime;

        if (stop(budget, out.spent, Phase::Bottom))
        {
            break;
        }
    }
    return out;
}

/// Starting ages spread so no two groups tie, and per-group costs that span the range a
/// real machine's variability spans. Only 1 group in 5 reports work on any given tick,
/// which is the regime in which a budget can bind at all: a set whose first group always
/// thought would exit after one iteration and could detect nothing.
std::vector<Group> MakeGroups()
{
    std::vector<Group> groups(kGroups);
    for (int j = 0; j < kGroups; j++)
    {
        groups[j].time = -static_cast<float>((j * 7) % 11) - 0.5f;
        groups[j].cost = 1 + (j % 5) * 3;
    }
    return groups;
}

/// Run `kTicks` center ticks, recording the selection into a timeline.
void RecordRun(StateTimeline& tl, int costScale, StopPolicy stop)
{
    std::vector<Group> groups = MakeGroups();
    float centerTime = 0.0f;

    const auto sel = tl.Intern("ai.groupScan");
    const auto fIdx = tl.Intern("visitedGroup");
    const auto fCount = tl.Intern("visitedCount");

    for (int t = 0; t < kTicks; t++)
    {
        const float simTime = static_cast<float>(t) * 0.1f;
        tl.BeginTick();

        const WalkResult r = Walk(groups, centerTime, simTime, t, costScale, ThinksByState, stop);

        // Discrete on purpose: which group thought is a selection outcome, and SIM-808's
        // rule is that no diagnostic may ever be applied to one.
        tl.U32(sel, 0, fCount, static_cast<std::uint32_t>(r.visited.size()));
        for (std::size_t k = 0; k < r.visited.size(); k++)
        {
            tl.U32(sel, static_cast<std::uint32_t>(k), fIdx, static_cast<std::uint32_t>(r.visited[k]));
        }

        // NOTE: the center clock is deliberately NOT advanced here. `_lastUpdateTime` is
        // advanced only on the `maxIdx < 0` path, inside the walk -- that is the source's
        // own cycle mechanism (AICenterImplPreview.cpp:509), and it is what spreads one
        // pass over the group list across several ticks.
        tl.EndTick();
    }
}

} // namespace

TEST_CASE("the deterministic group scan selects the same groups under any machine speed", "[determinism][ai][sim813]")
{
    // Two runs of identical simulation state, differing only in how expensive each Think()
    // was. Under the shipped policy that difference must be invisible.
    StateTimeline fast;
    StateTimeline slow;
    RecordRun(fast, 1, StopDeterministic);
    RecordRun(slow, 64, StopDeterministic);

    const auto div = CompareTimelines(fast, slow);
    INFO(div.Describe());
    CHECK(div.kind == DivergenceKind::None);
    CHECK(fast.RunHash() == slow.RunHash());

    // SIM-808's anti-vacuity rule. A timeline whose ticks all hash alike would agree with
    // any implementation at all, including a wrong one.
    CHECK(fast.TickCount() == kTicks);
    INFO("distinct tick hashes: " << fast.DistinctTickHashes());
    CHECK(fast.DistinctTickHashes() >= kTicks / 4);

    // And the walk must actually visit more than one group somewhere, or the budget could
    // never have bound and the comparison would be trivially satisfied.
    bool sawMultiVisit = false;
    for (int t = 0; t < fast.TickCount(); t++)
    {
        if (fast.EntryCountInTick(t) > 2)
        {
            sawMultiVisit = true;
        }
    }
    CHECK(sawMultiVisit);
}

TEST_CASE("ABLATION: the wall-clock budget makes the selection a function of machine speed",
          "[determinism][ai][sim813]")
{
    // The same two runs under the PRE-SIM-813 policy. This is the defect, reproduced, and
    // it is the reason the case above is not vacuous: swap the policy and it fails.
    StateTimeline fast;
    StateTimeline slow;
    RecordRun(fast, 1, StopWallClock);
    RecordRun(slow, 64, StopWallClock);

    const auto div = CompareTimelines(fast, slow);
    INFO(div.Describe());
    CHECK(div.Diverged());
    // The first divergence must be a selection outcome, not a shape artefact of a shorter
    // run: the slow machine visits fewer groups in the very first tick.
    CHECK(div.tick == 0);
    CHECK(div.fieldKind == Poseidon::Determinism::FieldKind::Discrete);
}

TEST_CASE("the scan budget cannot bind on a walk that only visits existing groups", "[determinism][ai][sim813]")
{
    // The equivalence claim that makes SIM-813 a no-op on a fast machine: NGroups()+1 loop
    // entries is above the walk's own natural ceiling, so the shipped budget never cuts a
    // walk that a fast machine would have completed.
    //
    // Driven with a group set where NOTHING reports work, which is the walk's worst case:
    // every group is visited before the selection runs out.
    std::vector<Group> groups(kGroups);
    for (int j = 0; j < kGroups; j++)
    {
        groups[j].time = -1.0f - static_cast<float>(j);
    }
    float centerTime = 0.0f;
    WalkResult r = Walk(groups, centerTime, 1.0f, 0, 1, ThinksNever, StopDeterministic);

    // Every group visited exactly once, then the selection found nothing and ended the
    // cycle on its own -- not on the budget.
    CHECK(r.visited.size() == static_cast<std::size_t>(kGroups));
    CHECK(centerTime == 1.0f); // set only on the `maxIdx < 0` path

    // No index repeats: stamping really does remove a group from the running.
    std::vector<bool> seen(kGroups, false);
    for (int idx : r.visited)
    {
        CHECK(!seen[idx]);
        seen[idx] = true;
    }
}

TEST_CASE("the scan budget bounds a walk that creates groups under it", "[determinism][ai][sim813]")
{
    // The case the wall clock was really catching. `AIGroup::AIGroup` sets
    // `_lastUpdateTime = Glob.time - 1.0F`, so a group created inside Think() is
    // immediately one second old and is picked next -- an unbounded walk with no clock to
    // stop it. The budget bounds it deterministically.
    GroupScanBudget budget(kGroups);
    int entries = 0;
    while (!budget.Exhausted())
    {
        entries++;
        if (entries > 10000)
        {
            break; // the budget failed to bind; the CHECK below reports it
        }
    }
    CHECK(entries == kGroups + 1);
    CHECK(budget.Visited() == kGroups + 1);
    CHECK(budget.Ceiling() == kGroups + 1);

    // Degenerate centers must still terminate.
    GroupScanBudget empty(0);
    CHECK(empty.Ceiling() == 1);
    CHECK_FALSE(empty.Exhausted());
    CHECK(empty.Exhausted());
}
