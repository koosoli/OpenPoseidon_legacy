#pragma once

// Roadmap Phase 8 -- "Jobs should produce results or mutation commands. They should not
// directly modify shared world state. Commit order must be deterministic and suitable for
// replay testing." See
// design notes
//
// SIM-811 built the serial task graph and closed by saying its resource declarations are
// "descriptive rather than binding", because every stage still mutates in place. This is
// the ordering primitive that binding would rest on, and ONLY that: a queue of mutation
// records plus a total order over them that does not depend on the order the records were
// produced in.
//
// NOTHING IN THE ENGINE ENQUEUES INTO THIS YET. It is deliberately unwired -- SIM-812
// audited the two large stages and found that the obvious candidate (the landscape cell
// relink) is read back WITHIN its own stage by the collision queries, so deferring it
// would change behaviour rather than preserve it. Wiring is specified in the decision
// record, not performed here.
//
// ---------------------------------------------------------------------------
// The key, and why it has these four fields
// ---------------------------------------------------------------------------
//
// Phase 8 forbids a commit order that depends on scheduler order. Each field removes one
// specific way scheduler order could leak in:
//
//   `stage` -- which SimStage produced it (SIM-811's `kSimStageOrder` position). Keeps
//              stages from interleaving when they eventually overlap in time.
//   `site`  -- which write path. All position writes commit together, then all damage
//              writes. Makes a replay log legible and groups commutative writes.
//   `job`   -- WHICH JOB, assigned from the work item's index in a work list enumerated
//              deterministically BEFORE dispatch. This is the load-bearing field: it is
//              not "which worker picked it up" and not "which finished first".
//   `seq`   -- the job's own emission counter, so a job that writes the same field twice
//              keeps its own internal order. Without it a job's writes could be reordered
//              against each other, which no amount of care elsewhere would fix.
//
// Together these are UNIQUE per record, which is what makes the commit independent of the
// sort algorithm's stability -- see `HasDuplicateKeys`, which a test should assert.
//
// ---------------------------------------------------------------------------
// The comparator, and the defect it is written against
// ---------------------------------------------------------------------------
//
// SIM-803 found a live comparator in this engine that ranked by `info1->_idExact -
// info2->_idExact` -- a pointer subtraction narrowed to `int`. Three objects spanning
// slightly over 2 GiB produced A<B, B<C, C<A: a CYCLE, in shipping AI code that decided
// which group guarded which vehicle.
//
// `CommitKeyBefore` is therefore written as field-by-field `<` comparisons and CONTAINS NO
// SUBTRACTION ANYWHERE. That is not stylistic. A `job` field is a 32-bit ordinal, and
// `(int)(a.job - b.job)` would reproduce SIM-803's bug exactly for ordinals more than
// 2^31 apart. Do not "simplify" this to a subtraction or a spaceship over a packed int.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Poseidon::Determinism
{

/// Where one mutation record sits in the deterministic total order.
struct CommitKey
{
    /// SimStage ordinal (SIM-811 `kSimStageOrder` position).
    std::uint8_t stage = 0;
    /// Which write path within the stage.
    std::uint16_t site = 0;
    /// The emitting job's index in the pre-dispatch work list. NEVER a worker id, a
    /// completion ordinal, or a pointer.
    std::uint32_t job = 0;
    /// The emitting job's own monotonic emission counter.
    std::uint32_t seq = 0;
};

/// Strict total order over `CommitKey`. No subtraction -- see the header comment.
///
/// Irreflexive, antisymmetric and transitive; `test_deterministic_commit.cpp` asserts all
/// three exhaustively over a key space that includes ordinals far enough apart to trigger
/// SIM-803's truncation bug had a subtraction been used.
[[nodiscard]] bool CommitKeyBefore(const CommitKey& a, const CommitKey& b);

/// True when every field matches.
[[nodiscard]] bool CommitKeyEqual(const CommitKey& a, const CommitKey& b);

/// One deferred mutation.
///
/// The payload is deliberately opaque: a `target` the applier resolves and 64 raw bits.
/// Bits, not values, for the same reason `StateTimeline` stores bits -- a queue that
/// round-trips a float through a wider type is a queue that can perturb a replay.
struct CommitRecord
{
    CommitKey     key{};
    std::uint32_t target = 0;
    std::uint64_t bits = 0;
};

/// A stage's mutation commands, committed in a deterministic order.
///
/// Single-threaded as written. Phase 8's shape is one queue per job merged before commit,
/// or one queue behind a lock; either way the ORDER comes from the key and never from the
/// order records arrived in, which is the property this class exists to provide.
class CommitQueue
{
public:
    void Enqueue(const CommitKey& key, std::uint32_t target, std::uint64_t bits);

    [[nodiscard]] std::size_t Size() const { return _records.size(); }
    [[nodiscard]] bool        Empty() const { return _records.empty(); }
    void                      Clear();

    /// Records in the order they were ENQUEUED. For tests and for diagnostics; the commit
    /// deliberately does not use this order.
    [[nodiscard]] const std::vector<CommitRecord>& Pending() const { return _records; }

    /// True if two pending records share a key. That is a BUG in the caller's key
    /// assignment, not something this class resolves: with a duplicate present the commit
    /// order depends on the sort's stability, which is exactly the dependence the key is
    /// supposed to remove. Assert it is false rather than relying on `std::stable_sort`.
    [[nodiscard]] bool HasDuplicateKeys() const;

    using CommitApply = void (*)(void* context, const CommitRecord& record);

    /// Sorts into `CommitKeyBefore` order and applies every record, in that order.
    /// Returns the number applied. The queue is left sorted and NOT cleared, so a test can
    /// inspect what order was used; call `Clear` to reuse it.
    std::size_t Commit(void* context, CommitApply apply);

private:
    std::vector<CommitRecord> _records;
};

} // namespace Poseidon::Determinism
