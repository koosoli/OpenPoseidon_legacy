// SIM-812 -- roadmap Phase 8: "Jobs should produce results or mutation commands. They
// should not directly modify shared world state. Commit order must be deterministic and
// suitable for replay testing."
//
// Two things are under test and they are not the same thing.
//
//   1. THE COMPARATOR IS A TOTAL ORDER. SIM-803 found a shipping comparator in this engine
//      that contained a CYCLE, because it subtracted two pointers and narrowed the result
//      to `int`. `CommitKeyBefore` is asserted irreflexive, antisymmetric and transitive
//      over a key space that INCLUDES ordinals more than 2^31 apart -- the exact operands
//      that broke SIM-803's version. A comparator that is merely "obviously fine" is how
//      that defect shipped.
//
//   2. THE COMMIT IS INDEPENDENT OF ARRIVAL ORDER. This is the Phase 8 property. A model
//      of a stage is run twice -- once mutating in place in work-list order, once by
//      enqueuing commands whose ARRIVAL ORDER IS SHUFFLED and committing them -- and the
//      two runs are compared with `Determinism::CompareTimelines`, which reports the first
//      divergence as a tick, a record, a field and two bit patterns.
//
//      The shuffle is the whole point. An equivalence test that enqueued in work-list
//      order would pass with no sort at all and would prove nothing.
//
// Every equivalence case has an ablation beside it, per SIM-806/SIM-808/SIM-811.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Core/CommitQueue.hpp>
#include <Poseidon/Core/StateTimeline.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

using Poseidon::Determinism::CommitKey;
using Poseidon::Determinism::CommitKeyBefore;
using Poseidon::Determinism::CommitKeyEqual;
using Poseidon::Determinism::CommitQueue;
using Poseidon::Determinism::CommitRecord;
using Poseidon::Determinism::CompareTimelines;
using Poseidon::Determinism::DivergenceKind;
using Poseidon::Determinism::StateTimeline;

namespace
{

// ---------------------------------------------------------------------------
// A model of one stage.
//
// Deliberately ORDER-SENSITIVE. `state[t] = state[t] * 0.5f + v` is neither commutative
// nor associative, so two runs that apply the same writes in different orders diverge in
// the bits. A model built on `+=` alone would agree with itself under any permutation and
// would prove nothing about ordering.
// ---------------------------------------------------------------------------

constexpr int kTargets = 4;
constexpr int kJobs = 12;
constexpr int kTicks = 64;
constexpr std::uint16_t kSite = 3;
constexpr std::uint8_t  kStage = 2; // SimStage::AI's ordinal in SIM-811's kSimStageOrder

struct Model
{
    float state[kTargets] = {};
};

/// The value job `j` writes on its `w`-th write of tick `tick`. Pure, so both runs
/// necessarily generate identical write SETS and only the ORDER can differ.
///
/// The `tick * 0.01f` drift is not decoration. `state = state * 0.5 + v` is a decaying
/// filter, so a purely periodic input drives the state periodic too -- the first version
/// of this model produced only 41 distinct tick hashes out of 64 and failed SIM-808's
/// anti-vacuity rule. A run whose ticks repeat would agree with a wrong implementation on
/// every repeated tick.
float WriteValue(int tick, int j, int w)
{
    return 0.125f * static_cast<float>((tick * 7 + j * 13 + w * 29) % 17) - 1.0f + static_cast<float>(tick) * 0.01f;
}

/// Every write of one job targets the SAME slot, and `kJobs > kTargets` so several jobs
/// share each slot.
///
/// Both halves are load-bearing and the first was a bug in the first version of this
/// model. When a job's writes went to DIFFERENT slots, their relative order could not
/// matter, and the ablation that drops `seq` from the key passed as "no divergence" --
/// wrongly suggesting `seq` was unnecessary. Writes to distinct slots commute; only writes
/// that collide can detect an ordering error.
int WriteTarget(int j, int /*w*/)
{
    return j % kTargets;
}

/// Writes per job. Varies by job so `seq` genuinely has work to do.
int WritesForJob(int j)
{
    return 1 + (j % 3);
}

void ApplyWrite(Model& model, int target, float value)
{
    model.state[target] = model.state[target] * 0.5f + value;
}

/// A deterministic shuffle. Not `std::random_device` -- a test whose ablation depends on
/// the machine's entropy is not a test. This LCG stands in for "jobs completed in some
/// order that is not the work-list order", which is exactly what a thread pool produces.
std::vector<int> ShuffledIndices(std::size_t n, std::uint32_t seed)
{
    std::vector<int> order;
    order.reserve(n);
    for (std::size_t i = 0; i < n; i++)
    {
        order.push_back(static_cast<int>(i));
    }
    std::uint32_t s = seed | 1u;
    for (std::size_t i = n; i > 1; i--)
    {
        s = s * 1664525u + 1013904223u;
        const std::size_t j = s % i;
        std::swap(order[i - 1], order[j]);
    }
    return order;
}

void RecordTick(StateTimeline& timeline, const Model& model)
{
    for (int t = 0; t < kTargets; t++)
    {
        timeline.F32("state", static_cast<std::uint32_t>(t), "v", model.state[t]);
    }
}

// ---- run A: every job mutates shared state in place, in work-list order ----
void RunInPlace(StateTimeline& timeline)
{
    Model model;
    for (int tick = 0; tick < kTicks; tick++)
    {
        timeline.BeginTick();
        for (int j = 0; j < kJobs; j++)
        {
            for (int w = 0; w < WritesForJob(j); w++)
            {
                ApplyWrite(model, WriteTarget(j, w), WriteValue(tick, j, w));
            }
        }
        RecordTick(timeline, model);
        timeline.EndTick();
    }
}

struct ApplyContext
{
    Model* model = nullptr;
};

void ApplyRecord(void* context, const CommitRecord& record)
{
    auto* ctx = static_cast<ApplyContext*>(context);
    float value = 0.0f;
    const auto bits = static_cast<std::uint32_t>(record.bits);
    static_assert(sizeof(value) == sizeof(bits), "float is 32-bit");
    std::memcpy(&value, &bits, sizeof(value));
    ApplyWrite(*ctx->model, static_cast<int>(record.target), value);
}

std::uint64_t BitsOf(float v)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

/// Options that exist ONLY so the ablations can switch them off. Production callers get
/// the defaults.
struct CommitOptions
{
    /// Set `seq` on every key. Ablated to prove `seq` is load-bearing.
    bool useSeq = true;
    /// Sort before applying. Ablated to prove the sort is load-bearing.
    bool sort = true;
};

// ---- run B: every job ENQUEUES commands; arrival order is shuffled; then commit ----
void RunCommitted(StateTimeline& timeline, std::uint32_t shuffleSeed, CommitOptions options = {})
{
    Model        model;
    ApplyContext context;
    context.model = &model;

    for (int tick = 0; tick < kTicks; tick++)
    {
        timeline.BeginTick();

        // Build every job's commands, then present them to the queue in a SHUFFLED order:
        // the queue must not care when a job's records showed up.
        struct Pending
        {
            CommitKey     key;
            std::uint32_t target;
            std::uint64_t bits;
        };
        std::vector<Pending> pending;
        for (int j = 0; j < kJobs; j++)
        {
            for (int w = 0; w < WritesForJob(j); w++)
            {
                CommitKey key;
                key.stage = kStage;
                key.site = kSite;
                // `job` is the work-list index, decided BEFORE dispatch -- never a worker
                // id and never a completion ordinal.
                key.job = static_cast<std::uint32_t>(j);
                key.seq = options.useSeq ? static_cast<std::uint32_t>(w) : 0u;
                pending.push_back({key, static_cast<std::uint32_t>(WriteTarget(j, w)), BitsOf(WriteValue(tick, j, w))});
            }
        }

        CommitQueue      queue;
        std::vector<int> arrival = ShuffledIndices(pending.size(), shuffleSeed + static_cast<std::uint32_t>(tick));
        for (int idx : arrival)
        {
            queue.Enqueue(pending[idx].key, pending[idx].target, pending[idx].bits);
        }

        if (options.sort)
        {
            queue.Commit(&context, ApplyRecord);
        }
        else
        {
            // Ablation: apply in arrival order, which is what "no deterministic commit
            // order" actually means.
            for (const CommitRecord& record : queue.Pending())
            {
                ApplyRecord(&context, record);
            }
        }

        RecordTick(timeline, model);
        timeline.EndTick();
    }
}

} // namespace

// ===========================================================================
// 1. The comparator is a total order
// ===========================================================================

TEST_CASE("CommitKeyBefore is irreflexive and antisymmetric", "[determinism][commit]")
{
    // Ordinals chosen to span the range where a subtraction narrowed to `int` breaks:
    // SIM-803's defect was exactly `(int)(ptrdiff_t)` truncation for operands >2 GiB apart.
    const std::uint32_t jobs[] = {0u, 1u, 0x40000000u, 0x80000000u, 0xC0000000u, 0xFFFFFFFFu};
    const std::uint32_t seqs[] = {0u, 7u, 0x7FFFFFFFu, 0x80000001u, 0xFFFFFFFFu};

    std::vector<CommitKey> keys;
    for (std::uint8_t stage : {std::uint8_t{0}, std::uint8_t{2}, std::uint8_t{5}})
    {
        for (std::uint16_t site : {std::uint16_t{0}, std::uint16_t{3}, std::uint16_t{65535}})
        {
            for (std::uint32_t job : jobs)
            {
                for (std::uint32_t seq : seqs)
                {
                    CommitKey k;
                    k.stage = stage;
                    k.site = site;
                    k.job = job;
                    k.seq = seq;
                    keys.push_back(k);
                }
            }
        }
    }
    REQUIRE(keys.size() == 3 * 3 * 6 * 5);

    for (const CommitKey& a : keys)
    {
        // Irreflexive.
        REQUIRE_FALSE(CommitKeyBefore(a, a));
        for (const CommitKey& b : keys)
        {
            const bool ab = CommitKeyBefore(a, b);
            const bool ba = CommitKeyBefore(b, a);
            // Antisymmetric, and exactly one of {a<b, b<a, a==b} holds.
            REQUIRE_FALSE((ab && ba));
            REQUIRE((ab || ba) == !CommitKeyEqual(a, b));
        }
    }
}

TEST_CASE("CommitKeyBefore is transitive, including across the 2^31 boundary SIM-803 tripped on",
          "[determinism][commit]")
{
    // The specific shape that produced a CYCLE in AICenterImpl.cpp: three operands at
    // increasing values whose outer difference overflows a signed 32-bit result. If
    // CommitKeyBefore is ever "simplified" to a subtraction, this fails.
    const std::uint32_t jobs[] = {0u, 0x40000000u, 0x80000000u, 0xC0000000u, 0xFFFFFFFFu};

    std::vector<CommitKey> keys;
    for (std::uint32_t job : jobs)
    {
        for (std::uint32_t seq : {0u, 0x80000000u})
        {
            CommitKey k;
            k.stage = 1;
            k.site = 2;
            k.job = job;
            k.seq = seq;
            keys.push_back(k);
        }
    }

    int strictTriples = 0;
    for (const CommitKey& a : keys)
    {
        for (const CommitKey& b : keys)
        {
            if (!CommitKeyBefore(a, b))
            {
                continue;
            }
            for (const CommitKey& c : keys)
            {
                if (!CommitKeyBefore(b, c))
                {
                    continue;
                }
                strictTriples++;
                REQUIRE(CommitKeyBefore(a, c));
                // The cycle SIM-803 measured would show up precisely here.
                REQUIRE_FALSE(CommitKeyBefore(c, a));
            }
        }
    }
    // Anti-vacuity: a transitivity test over a set where no a<b<c triple exists passes
    // trivially. Require that the loop actually did work.
    REQUIRE(strictTriples > 100);
}

TEST_CASE("a duplicate key is detected rather than silently resolved by the sort", "[determinism][commit]")
{
    CommitQueue queue;
    CommitKey   key;
    key.stage = 1;
    key.site = 1;
    key.job = 4;
    key.seq = 0;

    queue.Enqueue(key, 0, 1);
    REQUIRE_FALSE(queue.HasDuplicateKeys());

    // Same job, same seq -- the caller forgot to increment. With a duplicate present the
    // outcome depends on the sort's stability, which is the dependence the key exists to
    // remove, so this must be reported and not quietly tolerated.
    queue.Enqueue(key, 1, 2);
    REQUIRE(queue.HasDuplicateKeys());

    key.seq = 1;
    CommitQueue fixed;
    CommitKey   first = key;
    first.seq = 0;
    fixed.Enqueue(first, 0, 1);
    fixed.Enqueue(key, 1, 2);
    REQUIRE_FALSE(fixed.HasDuplicateKeys());
}

// ===========================================================================
// 2. Serial equivalence under a shuffled arrival order
// ===========================================================================

TEST_CASE("committing shuffled commands reproduces in-place mutation bit for bit", "[determinism][commit]")
{
    StateTimeline inPlace;
    RunInPlace(inPlace);

    // Anti-vacuity, SIM-808's rule: a model whose ticks all hash alike would agree with
    // anything. Every tick must be distinct.
    REQUIRE(inPlace.TickCount() == kTicks);
    REQUIRE(inPlace.DistinctTickHashes() == static_cast<std::size_t>(kTicks));

    // Several different shuffles: passing for one permutation could be luck.
    for (std::uint32_t seed : {1u, 12345u, 0xDEADBEEFu, 99u})
    {
        StateTimeline committed;
        RunCommitted(committed, seed);

        const auto d = CompareTimelines(inPlace, committed);
        INFO("shuffle seed " << seed << ": " << d.Describe());
        REQUIRE(d.kind == DivergenceKind::None);
        REQUIRE(inPlace.RunHash() == committed.RunHash());
    }
}

// ===========================================================================
// 3. Ablations -- a test that cannot fail is worthless
// ===========================================================================

TEST_CASE("ABLATION: applying in arrival order instead of committing diverges", "[determinism][commit]")
{
    StateTimeline inPlace;
    RunInPlace(inPlace);

    CommitOptions noSort;
    noSort.sort = false;

    StateTimeline arrivalOrder;
    RunCommitted(arrivalOrder, 12345u, noSort);

    const auto d = CompareTimelines(inPlace, arrivalOrder);
    INFO(d.Describe());
    // This is the control on the whole suite: without the sort, the shuffled arrival order
    // reaches the state and the two runs stop agreeing. If this ever passes as identical,
    // the model is not order-sensitive and the equivalence case above proves nothing.
    REQUIRE(d.Diverged());
    REQUIRE(d.kind == DivergenceKind::FieldValue);
}

TEST_CASE("ABLATION: dropping seq from the key loses a job's internal write order", "[determinism][commit]")
{
    StateTimeline inPlace;
    RunInPlace(inPlace);

    CommitOptions noSeq;
    noSeq.useSeq = false;

    // With seq always 0, every record of one job shares a key. The sort can no longer
    // order a job's own writes against each other, so it falls back to arrival order --
    // which is shuffled. `seq` is what makes the key unique, and this is the measurement
    // that says so rather than the comment claiming it.
    StateTimeline dropped;
    RunCommitted(dropped, 12345u, noSeq);

    const auto d = CompareTimelines(inPlace, dropped);
    INFO(d.Describe());
    REQUIRE(d.Diverged());
}

TEST_CASE("ABLATION: a perturbed value names its own tick and target", "[determinism][commit]")
{
    // The control on "first": a comparison that always answered with an early tick, or
    // with whatever tick it was handed, would pass a test that only ever perturbs tick 0.
    StateTimeline reference;
    RunInPlace(reference);

    StateTimeline perturbed;
    {
        Model model;
        for (int tick = 0; tick < kTicks; tick++)
        {
            perturbed.BeginTick();
            for (int j = 0; j < kJobs; j++)
            {
                for (int w = 0; w < WritesForJob(j); w++)
                {
                    float v = WriteValue(tick, j, w);
                    // Job 11 is the LAST writer to its slot (11 % kTargets == 3, and no
                    // later job shares it), and w == 2 is its last write.
                    //
                    // That precision is a measured requirement, not fussiness. This
                    // ablation first perturbed job 5's FIRST write and reported "no
                    // divergence": `state = state * 0.5 + v` halves any perturbation on
                    // every subsequent write to the same slot, and five further writes
                    // land on slot 1 within the tick, so one ulp is attenuated by 0.5^5
                    // and rounds away entirely before the tick is recorded. The model
                    // absorbs a late-cancelled error, which is a property of the model and
                    // not of `CompareTimelines`.
                    if (tick == 47 && j == 11 && w == 2)
                    {
                        // One ulp, at one tick, in one job's last write.
                        std::uint32_t bits = 0;
                        std::memcpy(&bits, &v, sizeof(bits));
                        bits += 1;
                        std::memcpy(&v, &bits, sizeof(v));
                    }
                    ApplyWrite(model, WriteTarget(j, w), v);
                }
            }
            RecordTick(perturbed, model);
            perturbed.EndTick();
        }
    }

    const auto d = CompareTimelines(reference, perturbed);
    INFO(d.Describe());
    REQUIRE(d.Diverged());
    // The tick must be the tick that was perturbed -- not 0, not the last one.
    REQUIRE(d.tick == 47);
    REQUIRE(d.kind == DivergenceKind::FieldValue);
    // One ulp is a DIVERGENCE. There is no tolerance path that could swallow it.
    REQUIRE(d.field == "v");
}
