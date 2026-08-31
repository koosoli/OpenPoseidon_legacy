#pragma once

// SIM-815 -- the per-TICK state recorder, one level below the frame hash.
//
// REN-THR-005 established that under POSEIDON_LOCKSTEP_HZ three runs of one
// mission take an IDENTICAL number of fixed steps in every frame and the
// published frames still differ in 28 of 40 -- and only in the
// position-dependent slices. The frame hash can say WHICH SLICE parted; it
// cannot say which simulation state moved first, because by the time a frame
// is published the divergence has already been through the camera, the LOD
// selector and the draw-list build. This recorder answers the question the
// frame hash cannot: at WHICH TICK, and in WHICH FIELD CLASS, did two runs'
// simulation state first disagree.
//
// It is the same discipline as EngineWgpu::HashPublishedFrame (FNV-1a,
// length-prefixed folds, per-class attribution, a distinct-hash anti-vacuity
// count) applied to simulation state instead of the published frame, and the
// same rule as StateTimeline (SIM-808): a recorder that reports a constant is
// measuring nothing, so every log line carries its own distinct-hash count.
//
// Two kinds of field, deliberately:
//
//  * HASHED CLASSES (positions, velocities, camera) -- many floats folded to
//    one 64-bit value per class per tick. Bits, never values: -0.0f == 0.0f
//    and NaN != NaN, so a value fold would be simultaneously too loose and
//    too tight. The fold is length-prefixed per item and count-suffixed per
//    tick, so two ticks with different entity partitions cannot collide by
//    concatenation.
//
//  * RAW COUNTERS (sequential RNG draws, resident shape count, entity count,
//    cloudlet count) -- logged as plain numbers, not hashed, because their
//    VALUE is the diagnosis. Two runs whose rng counters part at tick K have
//    a draw-count divergence at tick K; no hash comparison needed, and the
//    magnitude of the gap says how many draws one run took that the other did
//    not.
//
// This header is std-only. The engine hook (World::StepSimulation) supplies
// the values; nothing here knows what an Entity is, which is what makes the
// fold and the registry unit-testable without game data.

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace Poseidon::Determinism
{

/// The hashed field classes. Order is the log-line order and must not change
/// between builds that are to be diffed against each other.
enum class TickFieldClass : std::uint8_t
{
    Positions,  ///< every recorded entity's position, in iteration order
    Velocities, ///< every recorded entity's velocity, in the same order
    Camera,     ///< the scene camera's position
    Count_,
};

/// The raw counters. Same stability rule as TickFieldClass.
enum class TickCounter : std::uint8_t
{
    RngDraws,    ///< GRandGen's sequential draw counter (cumulative)
    ShapeCount,  ///< resident shapes in the ShapeBank
    EntityCount, ///< entities folded into Positions this tick
    Cloudlets,   ///< live cloudlet effects (the smoke spawn-rate witness)
    TimeMs,      ///< Glob.time in milliseconds -- the cross-run alignment key
    Count_,
};

inline constexpr std::size_t kTickFieldClassCount = static_cast<std::size_t>(TickFieldClass::Count_);
inline constexpr std::size_t kTickCounterCount = static_cast<std::size_t>(TickCounter::Count_);

/// One tick's summary -- what one log line carries.
struct TickStateRow
{
    std::uint64_t tick = 0;
    std::uint64_t classHash[kTickFieldClassCount] = {};
    std::uint64_t counter[kTickCounterCount] = {};
    std::uint64_t all = 0;          ///< fold of every class hash and every counter
    std::uint64_t distinct = 0;     ///< distinct `all` values seen so far, this run
};

class TickStateHash
{
public:
    /// Open a tick. Resets the per-class accumulators; the tick index and the
    /// distinct set persist across ticks.
    void BeginTick();

    /// Fold one item's bytes into a field class. Length-prefixed: an item of
    /// 12 bytes followed by one of 4 can never collide with 8 followed by 8.
    void FoldBytes(TickFieldClass cls, const void* p, std::size_t n);

    /// Convenience: fold three floats (a position or a velocity) as one item.
    void FoldVec3(TickFieldClass cls, float x, float y, float z);

    /// Set a raw counter for this tick. Later calls overwrite.
    void SetCounter(TickCounter c, std::uint64_t v);

    /// Close the tick: suffix each class fold with its item count, fold the
    /// whole row, update the distinct set, bump the tick index, and return
    /// the finished row.
    TickStateRow EndTick();

    [[nodiscard]] bool TickOpen() const { return _open; }
    [[nodiscard]] std::uint64_t TickIndex() const { return _tick; }

    /// Anti-vacuity (SIM-808's rule): a recorder whose ticks all hash alike is
    /// measuring nothing. Callers should require this to grow in any run where
    /// the world is live.
    [[nodiscard]] std::size_t DistinctTickHashes() const { return _distinct.size(); }

    /// One line for the log, grep key "TickHash". Stable column order; two
    /// runs' lines diff by the tms= key (Glob.time is affine in the tick index
    /// under lockstep, so equal tms means the same simulated moment).
    [[nodiscard]] static std::string FormatRow(const TickStateRow& row);

private:
    std::uint64_t _acc[kTickFieldClassCount] = {};
    std::uint32_t _items[kTickFieldClassCount] = {};
    std::uint64_t _counters[kTickCounterCount] = {};
    std::uint64_t _tick = 0;
    bool          _open = false;
    std::unordered_set<std::uint64_t> _distinct;
};

} // namespace Poseidon::Determinism
