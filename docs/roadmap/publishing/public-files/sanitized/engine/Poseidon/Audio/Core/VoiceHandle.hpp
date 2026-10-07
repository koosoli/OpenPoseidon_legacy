#pragma once

// Stable, generation-checked voice handles (roadmap parallel track A, A.2).
//
// WHY THIS EXISTS
// ---------------
// Audio identifies a live voice by raw `WaveOAL*`. A raw pointer answers "is
// there an object here" and nothing else, so every place that has to remember a
// voice ACROSS TIME has had to invent its own liveness proof. The one that
// existed before this file — SoundSystemOAL::ResumeMusicForPreview — cross-checked
// a remembered `WaveOAL*` against the live `_waves` registry and commented that
// "membership there proves the pointer is still valid". It does not. It proves
// that SOME live wave sits at that address. Free a suppressed music wave, let the
// allocator hand the same block to the next wave of the same size class, and the
// find() succeeds against a DIFFERENT voice, which then has its preview-mute
// cleared and its gain rewritten. That is ABA, and no pointer comparison can see
// it.
//
// A generation counter can. A handle carries the slot AND the generation that
// slot held when the handle was issued; releasing a slot bumps its generation, so
// every handle outstanding at that moment is dead from that instant, whatever the
// allocator does with the memory afterwards.
//
// SHAPE — deliberately identical to the renderer's InstanceTable
// -------------------------------------------------------------
// engine/WgpuRenderer/rust/src/gfx3d/cull.rs, `InstanceTable`, uses
// `(slot + 1) | (generation << 24)`. That layout is copied here verbatim rather
// than reinvented, so the two subsystems' identity rules read the same and a
// reviewer only has to learn them once. See design notes
//
//   bits  0..23 : slot + 1   -- so handle 0 is never issued and means "none"
//   bits 24..31 : generation -- 8 bits, per slot
//
// A stale handle can alias a live one only after 256 acquire/release cycles OF
// THE SAME SLOT. That bound is asserted by a test, not assumed.
//
// WHERE THIS DIFFERS FROM PresentationSnapshot
// --------------------------------------------
// PresentationSnapshot (Graphics/Rendering/Frame/) is a ring of by-value frame
// state with no handles in it at all, and its `resourceEpoch` is an unfed seam
// precisely because there is nothing in the payload an epoch could invalidate.
// The audio side is the opposite case: it has no snapshot yet, but it already has
// a second thread (SoundSystemOAL::StreamPumpLoop) holding raw voice pointers, so
// identity is the part that is load-bearing TODAY and the snapshot is the part
// that has no consumer yet. This file therefore does identity only.
//
// THREADING
// ---------
// The table is NOT internally synchronized, on purpose. Its owner already has a
// lock that covers exactly the same data (SoundSystemOAL::_audioMutex covers the
// `_waves` registry this table indexes), and a second, finer lock inside would
// only create a second order to get wrong. The caller must hold that lock across
// Acquire/Release/Resolve. Being lock-free of its own also keeps the table pure
// bookkeeping, which is what makes it testable with no device and no mixer — the
// lesson REN-ID-001 drew from a generation check that had no test because it could
// not be reached without a GPU.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Poseidon::Audio
{

// 0 is never issued by Acquire and always fails Resolve.
using VoiceHandle = uint32_t;
inline constexpr VoiceHandle kInvalidVoiceHandle = 0;

inline constexpr uint32_t kVoiceSlotMask = 0x00FF'FFFFu;
inline constexpr uint32_t kVoiceGenerationShift = 24u;
// The largest slot index that still fits once biased by +1.
inline constexpr uint32_t kMaxVoiceSlots = 0x00FF'FFFEu;

// Pack/unpack are free functions so a test can build a handle for a slot that was
// never allocated, which is one of the refusal reasons and needs its own case.
inline constexpr VoiceHandle MakeVoiceHandle(uint32_t slot, uint8_t generation)
{
    return (slot + 1u) | (static_cast<uint32_t>(generation) << kVoiceGenerationShift);
}
inline constexpr uint32_t VoiceHandleBiasedSlot(VoiceHandle h)
{
    return h & kVoiceSlotMask;
}
inline constexpr uint8_t VoiceHandleGeneration(VoiceHandle h)
{
    return static_cast<uint8_t>(h >> kVoiceGenerationShift);
}

// A slot table over non-owning pointers.
//
// It stores `T*` and never owns, dereferences or deletes them: lifetime stays
// with whoever created the voice. All this promises is that Resolve returns the
// pointer that was passed to Acquire for THIS handle, or nullptr — never a
// different object that happens to have inherited the address.
template <typename T>
class VoiceHandleTable
{
  public:
    // Registers `ptr` and returns a fresh handle. Returns kInvalidVoiceHandle if
    // the slot space is exhausted (counted in `exhaustions`), never a wrapped or
    // aliased handle.
    VoiceHandle Acquire(T* ptr)
    {
        uint32_t slot;
        if (!_freeSlots.empty())
        {
            slot = _freeSlots.back();
            _freeSlots.pop_back();
            ++_slotReuses;
        }
        else
        {
            if (_entries.size() >= kMaxVoiceSlots)
            {
                ++_exhaustions;
                return kInvalidVoiceHandle;
            }
            slot = static_cast<uint32_t>(_entries.size());
            _entries.push_back(Entry{});
        }
        Entry& e = _entries[slot];
        e.ptr = ptr;
        e.live = true;
        ++_liveCount;
        return MakeVoiceHandle(slot, e.generation);
    }

    // Returns the registered pointer, or nullptr if the handle is 0, out of
    // range, points at a free slot, or carries a stale generation. Every
    // refusal increments `staleOps` so a test can tell a rejection from a
    // no-op that happened to leave the data alone (REN-ID-001 §3).
    T* Resolve(VoiceHandle h) const
    {
        const Entry* e = Lookup(h);
        return e ? e->ptr : nullptr;
    }

    bool IsLive(VoiceHandle h) const { return Lookup(h) != nullptr; }

    // Frees the slot and bumps its generation, killing every handle outstanding
    // for it. Returns false (and counts a stale op) if the handle was already
    // dead — which is what stops a double release pushing one slot onto the free
    // list twice.
    bool Release(VoiceHandle h)
    {
        const Entry* found = Lookup(h);
        if (!found)
        {
            return false;
        }
        const uint32_t slot = VoiceHandleBiasedSlot(h) - 1u;
        Entry& e = _entries[slot];
        e.ptr = nullptr;
        e.live = false;
        // Wrapping is intended and bounded: see the 256-cycle test.
        e.generation = static_cast<uint8_t>(e.generation + 1u);
        _freeSlots.push_back(slot);
        --_liveCount;
        return true;
    }

    // Diagnostics. `staleOps` is readable, not just incremented — a write-only
    // counter is what made the renderer's equivalent check untestable.
    uint64_t StaleOps() const { return _staleOps; }
    uint64_t SlotReuses() const { return _slotReuses; }
    uint64_t Exhaustions() const { return _exhaustions; }
    int LiveCount() const { return _liveCount; }
    size_t SlotCount() const { return _entries.size(); }

    void Clear()
    {
        _entries.clear();
        _freeSlots.clear();
        _liveCount = 0;
        _staleOps = 0;
        _slotReuses = 0;
        _exhaustions = 0;
    }

  private:
    struct Entry
    {
        T* ptr = nullptr;
        uint8_t generation = 0;
        bool live = false;
    };

    const Entry* Lookup(VoiceHandle h) const
    {
        const uint32_t biased = VoiceHandleBiasedSlot(h);
        if (biased == 0u)
        {
            ++_staleOps;
            return nullptr;
        }
        const uint32_t slot = biased - 1u;
        if (slot >= _entries.size())
        {
            ++_staleOps;
            return nullptr;
        }
        const Entry& e = _entries[slot];
        // Order matters: a freed slot must be refused even when the caller's
        // generation still matches, because Release bumps AFTER clearing `live`
        // only for the slot it freed -- a never-acquired slot has generation 0
        // and would otherwise answer to a generation-0 handle.
        if (!e.live || e.generation != VoiceHandleGeneration(h))
        {
            ++_staleOps;
            return nullptr;
        }
        return &e;
    }

    std::vector<Entry> _entries;
    std::vector<uint32_t> _freeSlots;
    int _liveCount = 0;
    mutable uint64_t _staleOps = 0;
    uint64_t _slotReuses = 0;
    uint64_t _exhaustions = 0;
};

} // namespace Poseidon::Audio
