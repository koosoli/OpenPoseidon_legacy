#pragma once

// SIM-813 -- the deterministic replacement for the wall-clock budget that used to
// decide which AI groups think in a tick.
// See docs/roadmap/decisions/SIM-813-deterministic-ai-group-scan-20260831.md.
//
// `AICenter::UpdateGroup` walks its groups oldest-first, calling `AIGroup::Think()`
// until one of them reports it did real work. Until SIM-813 the walk was also cut
// short by `Foundation::CompareSectionTimeGE(sectionTime, 0.020f)` -- a
// QueryPerformanceCounter budget. That made the SET OF GROUPS THAT THINK a function
// of how fast the machine ran, so the AI stage was not deterministic before any
// parallelism was introduced, and no commit ordering downstream could make it so.
// SIM-812 ranked it above everything else its mutation audit found.
//
// What replaces it is a bound derived from simulation state alone: the number of
// groups the center holds. That number is the walk's own natural ceiling, because
//
//   * each iteration stamps exactly one group's `_lastUpdateTime` to the center's,
//   * the selection compares ages strictly (`maxAge < age`), so a stamped group has
//     age 0 and can never be picked again inside the same call, and
//   * a null slot is skipped without being stamped, so the stampable set is at most
//     `NGroups()`.
//
// So `NGroups() + 1` loop entries (the `+ 1` being the entry that finds nothing left
// and breaks on its own) is a limit that CANNOT bind in the ordinary case. The one
// way past it is a group CREATED inside `Think()` -- `AIGroup::AIGroup` sets
// `_lastUpdateTime = Glob.time - 1.0F` (`AIGroup.cpp:53`), an immediate age of one
// second -- which is the unbounded case the wall clock was really catching, and the
// case this bounds deterministically instead.
//
// The consequence, stated plainly because it is the whole trade: on a machine fast
// enough that a center's walk cost under 20 ms, behaviour is unchanged, iteration
// for iteration. On a machine or a tick where the old budget DID trip, more groups
// now think than used to. That is not a side effect, it is the fix -- "how many
// groups thought" can no longer answer "how fast is this CPU" -- and the worst case
// it admits is exactly the walk a fast machine already performs today.

#include <cstdint>

// NOTE: `Poseidon::AI` is a CLASS in this engine (`AI : public NetworkObject`,
// Path/PathPlanner.hpp:14), not a namespace, so this cannot live there. It sits with
// SIM-808's `StateTimeline` and SIM-812's `CommitQueue` instead, which is where the
// determinism primitives already are.
namespace Poseidon::Determinism
{

/// One `UpdateGroup` walk's deterministic budget.
///
/// Constructed from simulation state (`nGroups`), consumed once per loop entry.
/// Holds no clock, no counter of elapsed anything, and nothing that differs between
/// two runs of the same simulation state. That is its entire purpose.
class GroupScanBudget
{
  public:
    explicit GroupScanBudget(int nGroups) : _ceiling(nGroups > 0 ? nGroups + 1 : 1) {}

    /// Call once at the top of each loop entry. `true` means the walk must stop.
    ///
    /// Deliberately not `const`: it counts. A budget that could be queried without
    /// being spent would let a caller loop forever while believing it was bounded.
    bool Exhausted()
    {
        if (_visited >= _ceiling)
        {
            return true;
        }
        ++_visited;
        return false;
    }

    /// Loop entries made so far. For diagnostics and for tests; nothing gates on it.
    [[nodiscard]] int Visited() const { return _visited; }

    /// The ceiling this walk was given. NOT called `Limit`: `Limit` is a function-like
    /// macro in this engine (Foundation/Math/Math3DP.hpp:511), and a member of that name
    /// fails to compile in any translation unit that has seen the math headers. NOT called `Limit`: `Limit` is a
    /// function-like macro in this engine (Foundation/Math/Math3DP.hpp:511), and a member of that name fails to compile
    /// in any translation unit that has seen the math headers.
    [[nodiscard]] int Ceiling() const { return _ceiling; }

  private:
    int _ceiling = 1;
    int _visited = 0;
};

} // namespace Poseidon::Determinism
