#pragma once

#include <Poseidon/IO/Streams/QBStream.hpp>
#include <Poseidon/Graphics/Textures/PreparedDdsPublication.hpp>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace Poseidon::Streaming
{
// Optional scheduling experiment, not a payload cache. The caller supplies an already
// normalized store key and an immutable captured member. Suppression means only that a
// valid peer attempt existed at this observation; it never guarantees its eventual Put.
// The ordinary owner reader remains the fallback. No peer cancellation token is borrowed.
class PaaPreparationInflight
{
    struct Binding
    {
        std::string key;
        uint64_t generation;
        BankReadRequest source;
        render::DdsPublicationToken publication;
        uint64_t ticket = 0;
        size_t charge = 0;
        Binding(const std::string& name, uint64_t gen, const BankReadRequest& read,
                const render::DdsPublicationToken& token)
            : key(name), generation(gen), source(read), publication(token) {}
    };
  public:
    static constexpr size_t MaxSlots = 128;
    static constexpr size_t MetadataBudget = 1024 * 1024;
    static constexpr size_t MaxKeyBytes = 1023, MaxArchiveBytes = 8191, MaxLeaseBytes = 16384;
    enum class Outcome { Tracked, Suppressed, UnleasedOrInvalid, CapacityPassThrough, ContendedPassThrough };
    struct Statistics
    {
        uint64_t tracked = 0, suppressed = 0, unsupported = 0, overflow = 0, contended = 0;
        size_t active = 0, metadataBytes = 0, peakMetadataBytes = 0;
    };
  private:
    struct State
    {
        std::mutex mutex;
        std::array<std::shared_ptr<const Binding>, MaxSlots> slots{};
        uint64_t nextTicket = 0;
        Statistics statistics;
        size_t budget;
        explicit State(size_t cap) : budget(cap < MetadataBudget ? cap : MetadataBudget)
        { statistics.metadataBytes = statistics.peakMetadataBytes = sizeof(State) + 128; }
    };
    std::shared_ptr<State> _state;
  public:
    class Ticket
    {
        friend class PaaPreparationInflight;
        std::shared_ptr<State> _state;
        std::shared_ptr<const Binding> _binding;
        size_t _slot = MaxSlots;
        Ticket(std::shared_ptr<State> state, std::shared_ptr<const Binding> binding, size_t slot)
            : _state(std::move(state)), _binding(std::move(binding)), _slot(slot) {}
      public:
        Ticket() = default;
        Ticket(const Ticket&) = delete;
        Ticket& operator=(const Ticket&) = delete;
        Ticket(Ticket&& other) noexcept = default;
        Ticket& operator=(Ticket&& other) noexcept
        {
            if (this != &other) { Release(); _state = std::move(other._state); _binding = std::move(other._binding); _slot = other._slot; }
            return *this;
        }
        ~Ticket() { Release(); }
        bool OwnsSlot() const { return bool(_binding); }
        void Release()
        {
            if (!_state || !_binding) return;
            std::shared_ptr<const Binding> retired;
            {
                std::lock_guard<std::mutex> lock(_state->mutex);
                auto& entry = _state->slots[_slot];
                if (entry && entry->ticket == _binding->ticket)
                {
                    --_state->statistics.active;
                    retired = std::move(entry);
                }
                // Replaced stale tickets still retain their own binding/lease until
                // release; their charge never disappears just because a slot was reused.
                _state->statistics.metadataBytes -= _binding->charge;
            }
            // Lease/token/string destruction is always outside the tracker mutex.
            _binding.reset(); _state.reset();
        }
    };
    struct Result { Outcome outcome; Ticket ticket; };
    // Limits count known C++ object/string requested capacities plus conservative control
    // block guards, not allocator/kernel/RSS bytes. Each concurrent Acquire can
    // transiently own one <=~28KiB new binding/string allocation plus shared references
    // to a lease (<=16KiB known capacity) and inventory (<=1MiB checked metadata).
    // These transient references are additional to this bounded retained-ticket
    // metadata; a briefly copied peer can retain an old binding after its ticket
    // releases. Inventory/lease metadata is conservatively charged per binding,
    // despite sharing; this is not a process-wide heap limit.
    explicit PaaPreparationInflight(size_t metadataBudget = MetadataBudget)
        : _state(std::make_shared<State>(metadataBudget)) {}
    Statistics Snapshot() const
    { std::lock_guard<std::mutex> lock(_state->mutex); return _state->statistics; }
    Result Acquire(const std::string& normalizedKey, uint64_t storeGeneration,
                   const BankReadRequest& source, const render::DdsPublicationToken& publication)
    {
        // Both operations are pure immutable/atomic observations, never bank/VFS access.
        // Token checks deliberately happen outside our mutex (no queue-lock dependency).
        if (!storeGeneration || normalizedKey.empty() || !source.HasArchiveIdentity() || !publication.Valid())
            return PassThrough(Outcome::UnleasedOrInvalid);
        if (normalizedKey.size() > MaxKeyBytes || source.archive.size() > MaxArchiveBytes ||
            source.ArchiveIdentityBytes() > MaxLeaseBytes || publication.inventory->MetadataBytes() > MetadataBudget)
            return PassThrough(Outcome::CapacityPassThrough);
        std::shared_ptr<Binding> candidate;
        try { candidate = std::make_shared<Binding>(normalizedKey, storeGeneration, source, publication); }
        catch (...) { return PassThrough(Outcome::CapacityPassThrough); }
        if (candidate->key.capacity() > MaxKeyBytes || candidate->source.archive.capacity() > MaxArchiveBytes)
            return PassThrough(Outcome::CapacityPassThrough);
        candidate->charge = sizeof(Binding) + candidate->key.capacity() + 1 +
            candidate->source.archive.capacity() + 1 + candidate->source.ArchiveIdentityBytes() +
            candidate->publication.inventory->MetadataBytes() + 128;
        for (unsigned retry = 0; retry < 4; ++retry)
        {
            if (!publication.Valid()) return PassThrough(Outcome::UnleasedOrInvalid);
            std::shared_ptr<const Binding> peer;
            size_t index = MaxSlots;
            {
                std::lock_guard<std::mutex> lock(_state->mutex);
                size_t empty = MaxSlots;
                for (size_t i = 0; i < MaxSlots; ++i)
                {
                    const auto& entry = _state->slots[i];
                    if (!entry) { if (empty == MaxSlots) empty = i; continue; }
                    // Bounded field-only comparison: frozen strings/file identity/range,
                    // no native calls/read/hash/token validation or bank/queue lock.
                    if (entry->generation == storeGeneration && entry->key == normalizedKey &&
                        entry->source.SameArchiveMember(source)) { peer = entry; index = i; break; }
                }
                if (!peer) return InstallLocked(candidate, empty);
            }
            const bool peerValid = peer->publication.Valid();
            if (!publication.Valid()) return PassThrough(Outcome::UnleasedOrInvalid);
            {
                std::lock_guard<std::mutex> lock(_state->mutex);
                if (_state->slots[index] != peer) continue; // no ABA reuse of an expired ticket
                if (peerValid)
                {
                    ++_state->statistics.suppressed;
                    return {Outcome::Suppressed, {}};
                }
                // Cancellation never waits for the old worker. Replace bookkeeping;
                // its old ticket retains debt/lease until release and cannot erase ours.
                --_state->statistics.active;
                _state->slots[index].reset(); // peer keeps destruction outside lock
                return InstallLocked(candidate, index);
            }
        }
        return PassThrough(Outcome::ContendedPassThrough);
    }
  private:
    Result InstallLocked(const std::shared_ptr<Binding>& binding, size_t slot)
    {
        if (slot == MaxSlots || _state->statistics.metadataBytes > _state->budget ||
            binding->charge > _state->budget - _state->statistics.metadataBytes ||
            _state->nextTicket == std::numeric_limits<uint64_t>::max())
        { ++_state->statistics.overflow; return {Outcome::CapacityPassThrough, {}}; }
        binding->ticket = ++_state->nextTicket;
        _state->slots[slot] = binding;
        auto& stats = _state->statistics;
        ++stats.active; ++stats.tracked; stats.metadataBytes += binding->charge;
        if (stats.metadataBytes > stats.peakMetadataBytes) stats.peakMetadataBytes = stats.metadataBytes;
        return {Outcome::Tracked, Ticket(_state, binding, slot)};
    }
    Result PassThrough(Outcome outcome)
    {
        std::lock_guard<std::mutex> lock(_state->mutex);
        if (outcome == Outcome::UnleasedOrInvalid) ++_state->statistics.unsupported;
        else if (outcome == Outcome::CapacityPassThrough) ++_state->statistics.overflow;
        else ++_state->statistics.contended;
        return {outcome, {}};
    }
};
} // namespace Poseidon::Streaming
