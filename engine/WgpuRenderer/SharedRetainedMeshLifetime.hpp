#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

namespace Poseidon::render
{
// One engine lifetime, not a Rust pool generation. Close serializes the final
// destruction enqueue against renderer teardown; no draw uses this mutex.
class SharedMeshEpoch
{
public:
    bool IsOpen() const { return _open.load(std::memory_order_acquire); }
    void Close() { std::lock_guard lock(_mutex); _open.store(false, std::memory_order_release); }
    template<class Retire> void Dispatch(uint64_t handle, Retire&& retire)
    {
        if (!handle) return;
        std::lock_guard lock(_mutex);
        if (_open.load(std::memory_order_acquire)) retire(handle);
    }
private:
    std::atomic<bool> _open{true};
    std::mutex _mutex;
};

// One monotonic producer identity within an engine epoch. Borrowers include
// queued creates: cancellation/fallback must detach them explicitly. No Shape,
// slot pointer, renderer pointer or strong GPU owner escapes in this helper.
class SharedRetainedMeshLifetime
{
public:
    SharedRetainedMeshLifetime(uint64_t producer, std::shared_ptr<SharedMeshEpoch> epoch)
        : _producer(producer), _epoch(std::move(epoch)) {}
    uint64_t Producer() const { return _producer; }
    struct Owners { uint64_t boundBorrowers; bool producerOwned; };
    Owners SnapshotOwners()
    { std::lock_guard lock(_mutex); return {_boundBorrowers, _producerOwned}; }
    template<class Retire> void Dispatch(uint64_t handle, Retire&& retire)
    { if (_epoch) _epoch->Dispatch(handle, std::forward<Retire>(retire)); }
    // Returns the full generational renderer handle exactly once when no
    // producer or CPU borrower owns it. Caller enqueues retirement after unlock.
    uint64_t ReleaseProducer(uint64_t rendererHandle)
    {
        std::lock_guard lock(_mutex);
        if (!_epoch || !_epoch->IsOpen() || !_producerOwned) return 0;
        if (_handle && _handle != rendererHandle) return 0;
        _handle = rendererHandle;
        _producerOwned = false;
        return ClaimRetirement();
    }
    class Borrower
    {
    public:
        Borrower() = default;
        Borrower(const Borrower&) = delete;
        Borrower& operator=(const Borrower&) = delete;
        bool Attach(const std::shared_ptr<SharedRetainedMeshLifetime>& group)
        {
            if (!group || _group) return false;
            std::lock_guard lock(group->_mutex);
            if (!group->_producer || !group->_epoch || !group->_epoch->IsOpen() || !group->_producerOwned ||
                group->_borrowers == std::numeric_limits<uint64_t>::max()) return false;
            ++group->_borrowers;
            _group = group; _active = true;
            return true;
        }
        uint64_t Producer() const { return _group ? _group->Producer() : 0; }
        bool Bind(uint64_t rendererHandle)
        {
            if (!_group) return false;
            std::lock_guard lock(_group->_mutex);
            if (!_active || !_group->_epoch->IsOpen() || !rendererHandle ||
                (_group->_handle && _group->_handle != rendererHandle)) return false;
            _group->_handle = rendererHandle;
            if (!_bound) { ++_group->_boundBorrowers; _bound = true; }
            return true;
        }
        // False means standalone fallback: use the original slot destruction.
        // True also covers pre-create cancellation and closed-epoch late release.
        bool Release(uint64_t& retirement)
        {
            retirement = 0;
            if (_diagnosticStandaloneHandled.load(std::memory_order_acquire)) return true;
            if (!_group) return false;
            std::lock_guard lock(_group->_mutex);
            if (_standalone) return false;
            retirement = Detach();
            return true;
        }
        // Optional attributed destructor took responsibility for standalone
        // dispatch (including closed-epoch suppression). Does not change the
        // immutable shared-group association, renderer handle or draw state.
        void SuppressHandledStandaloneRetirement()
        { _diagnosticStandaloneHandled.store(true,std::memory_order_release); }
        uint64_t FallBackToStandalone()
        {
            if (!_group) return 0;
            std::lock_guard lock(_group->_mutex);
            _standalone = true;
            return Detach();
        }
        template<class Retire> void Dispatch(uint64_t handle, Retire&& retire)
        { if (_group) _group->Dispatch(handle, std::forward<Retire>(retire)); }
    private:
        uint64_t Detach()
        {
            if (!_active) return 0;
            _active = false;
            if (_bound) { --_group->_boundBorrowers; _bound = false; }
            --_group->_borrowers;
            return _group->ClaimRetirement();
        }
        // Immutable association after Attach. Only flags/counts under group lock
        // change during drain/fallback/destruction, not the shared_ptr itself.
        std::shared_ptr<SharedRetainedMeshLifetime> _group;
        bool _active = false, _standalone = false, _bound = false;
        std::atomic<bool> _diagnosticStandaloneHandled{false};
    };
    struct PreDiagnosticBorrowerLayout
    {
        std::shared_ptr<SharedRetainedMeshLifetime> group;
        bool active,standalone,bound;
    };
    static_assert(sizeof(std::atomic<bool>)==sizeof(bool), "diagnostic bit must fit original bool padding");
    static_assert(sizeof(Borrower)==sizeof(PreDiagnosticBorrowerLayout), "diagnostic bit must not grow normal borrower slots");
private:
    uint64_t ClaimRetirement()
    {
        if (_producerOwned || _borrowers || _retired || !_handle || !_epoch->IsOpen()) return 0;
        _retired = true;
        return _handle;
    }
    const uint64_t _producer;
    std::shared_ptr<SharedMeshEpoch> _epoch;
    std::mutex _mutex;
    uint64_t _handle = 0, _borrowers = 0, _boundBorrowers = 0;
    bool _producerOwned = true, _retired = false;
};
}
