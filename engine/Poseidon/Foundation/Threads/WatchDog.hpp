// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <string_view>
#include <thread>
#include <vector>

// A stall detector: name a scope on entry, and if it has not finished within the deadline a
// background sweep says so, with the name. It exists for the failures that produce no error at
// all -- a streaming admit that never returns, a shader compile that wedges, a join that
// deadlocks -- where the only symptom today is that the frame counter stops and there is
// nothing in the log to say which scope owned the thread.
//
// Three properties are deliberate, and each one is a bug or a hazard that the obvious
// implementation of this walks straight into:
//
//  1. IT DOES NOT PRINT TO `std::cerr`. On this workstation that is not a style question: the
//     wgpu renderer panics when the process writes to stderr under the agent shell
//     (`failed printing to stderr: os error 232`), and a background thread writing there at an
//     arbitrary moment is the worst possible version of that. Reports go through the normal
//     logging system instead, which is also where anyone looking for the stall will be reading.
//
//  2. IT DOES NOT START THE THREAD AT STATIC-INIT TIME. A namespace-scope instance is the
//     tempting shape and the wrong one: a std::thread spawned before main and joined during static destruction
//     is a well-known way to hang at exit, and it costs the ring's memory in every process that
//     links the header -- including the tools, which have no frames to watch. Here the sweep is
//     opt-in and explicitly owned: nothing runs until someone constructs one.
//
//  3. THE RING CANNOT BE RE-ENTERED UNDER A LIVE TOKEN. Taking the next slot modulo the ring
//     size unconditionally is the natural way to write `Add`, and it is wrong: once the counter
//     wraps past a slot whose scope is still running, two `WatchDogItem`s hold the same status
//     pointer and the first to finish clears the other's entry. With 2^17 slots that needs
//     131,072 nested-or-concurrent scopes to bite, which is why such a bug hides -- but a
//     diagnostic that lies under load is worse than none. Each slot carries a generation
//     counter; a token only clears the slot it was issued for.
//
// Cost when armed is one relaxed fetch_add and one release store per scope. Cost when not
// armed is nothing at all -- the ring is not allocated and no thread exists.
namespace Poseidon
{

class WatchDog;

/// Move-only RAII token for one watched scope. Finishing is idempotent; letting it go out of
/// scope finishes it.
class WatchDogItem
{
public:
    WatchDogItem() noexcept = default;

    WatchDogItem(const WatchDogItem&) = delete;
    WatchDogItem& operator=(const WatchDogItem&) = delete;

    WatchDogItem(WatchDogItem&& other) noexcept : _slot(other._slot), _generation(other._generation)
    {
        other._slot = nullptr;
    }

    WatchDogItem& operator=(WatchDogItem&& other) noexcept
    {
        if (this != &other)
        {
            Finish();
            _slot = other._slot;
            _generation = other._generation;
            other._slot = nullptr;
        }
        return *this;
    }

    ~WatchDogItem() noexcept { Finish(); }

    /// Mark the scope complete. Safe to call more than once, and safe after the ring has
    /// recycled this slot -- the generation check is what makes that true (see change 3 above).
    void Finish() noexcept;

private:
    friend class WatchDog;
    struct Slot;

    WatchDogItem(Slot& slot, uint32_t generation) noexcept : _slot(&slot), _generation(generation) {}

    Slot* _slot = nullptr;
    uint32_t _generation = 0;
};

/// One ring slot. `generation` is bumped every time the slot is handed out, and a token only
/// clears the slot while the generation still matches -- that is the whole of change 3.
struct WatchDogItem::Slot
{
    Slot() noexcept = default;
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    // std::vector needs this to grow; the ring is sized once at construction and never grows,
    // so the move is only ever the initial fill and copying the atomics' values is correct there.
    Slot(Slot&& other) noexcept
        : generation(other.generation.load(std::memory_order_relaxed)),
          running(other.running.load(std::memory_order_relaxed)), deadline(other.deadline), name(other.name)
    {
    }

    std::atomic<uint32_t> generation{0};
    std::atomic<bool> running{false};
    std::chrono::steady_clock::time_point deadline{};
    std::array<char, 32> name{};
};

class WatchDog
{
public:
    /// Ring size. A power of two so the index wrap is a mask. 2^16 slots is 64k scopes in
    /// flight before recycling, which the generation check now makes safe rather than merely
    /// unlikely.
    static constexpr size_t RingSize = static_cast<size_t>(1) << 16;
    static constexpr size_t RingMask = RingSize - 1;

    /// Longest a named scope may run before it is reported. The sweep runs at its own short
    /// cadence (capped at this limit for the very short limits tests use), so a stall is
    /// named within a fraction of a second of the deadline rather than within another limit.
    explicit WatchDog(std::chrono::milliseconds timeLimit);
    ~WatchDog() noexcept;

    WatchDog(const WatchDog&) = delete;
    WatchDog& operator=(const WatchDog&) = delete;

    /// Begin watching a scope. The name is copied into the slot (truncated to 31 characters
    /// plus a terminator) because the caller's string may be gone by the time the sweep reads
    /// it: a fixed-size inline buffer, not a pointer to storage the caller may free.
    [[nodiscard]] WatchDogItem Add(std::string_view name) noexcept;

    /// The process-wide instance, or nullptr when the watchdog was never armed.
    static WatchDog* Instance() noexcept { return _instance.load(std::memory_order_acquire); }

    /// How many scopes have been reported as overdue since construction. The counter is the
    /// part worth asserting on in a test; the log lines are for a human.
    uint64_t Reports() const noexcept { return _reports.load(std::memory_order_relaxed); }

private:
    // The ring's element type lives in WatchDogItem (the token is what has to name it); this
    // alias is what lets the implementation write `Slot` rather than repeating the qualification.
    using Slot = WatchDogItem::Slot;

    void Sweep() noexcept;

    std::atomic<bool> _running{true};
    std::atomic<size_t> _nextIndex{0};
    std::atomic<uint64_t> _reports{0};
    std::chrono::milliseconds _timeLimit;
    std::vector<WatchDogItem::Slot> _slots;
    std::thread _thread;

    static std::atomic<WatchDog*> _instance;
};

/// Watch the enclosing scope only when a watchdog is armed. Evaluates to an empty token
/// otherwise, so the call site costs a null check and nothing else.
inline WatchDogItem WatchScope(std::string_view name) noexcept
{
    WatchDog* dog = WatchDog::Instance();
    return dog ? dog->Add(name) : WatchDogItem();
}

/// Watch a scope whose interesting part is a path -- a file being read, a model being
/// parsed. Composes `label:<tail of detail>` into the slot's 32-character name.
///
/// The TAIL, not the head, and that is the whole reason this exists rather than callers
/// concatenating: asset paths here share long directory prefixes, so truncating from the
/// front yields 31 characters of `ca\data\...` for every scope and names nothing. The tail
/// is the file, which is the part that identifies the stall. Costs nothing when no
/// watchdog is armed -- the formatting happens behind the instance check.
[[nodiscard]] WatchDogItem WatchScopeFor(std::string_view label, std::string_view detail) noexcept;

} // namespace Poseidon
