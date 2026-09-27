#pragma once
#include "SharedRetainedMeshLifetime.hpp"
#include <memory>
#include <mutex>
#include <limits>
namespace Poseidon::render
{
// Optional diagnostic witness, not a GPU owner/ack. Full handle is bound only
// after actual creation. Cancellation may precede binding; late creation is
// retired exactly once via the same engine shutdown dispatch gate.
class StandaloneMeshRetirementWitness
{
public:
    struct State { uint64_t handle=0; bool released=false; };
    StandaloneMeshRetirementWitness(uint64_t producer,std::shared_ptr<SharedMeshEpoch> epoch)
        : _producer(producer),_epoch(std::move(epoch)) {}
    uint64_t Producer() const { return _producer; }
    uint64_t Bind(uint64_t handle)
    {
        std::lock_guard lock(_mutex);
        if(!handle || _handle) return 0;
        _handle=handle;
        return Claim();
    }
    uint64_t Release()
    { std::lock_guard lock(_mutex); _released=true; return Claim(); }
    State Snapshot() { std::lock_guard lock(_mutex); return {_handle,_released}; }
    template<class Retire> void Dispatch(uint64_t handle,Retire&& retire)
    { if(_epoch) _epoch->Dispatch(handle,std::forward<Retire>(retire)); }
private:
    uint64_t Claim()
    { if(!_released || !_handle || _claimed) return 0; _claimed=true; return _handle; }
    const uint64_t _producer;
    std::shared_ptr<SharedMeshEpoch> _epoch;
    std::mutex _mutex;
    uint64_t _handle=0;
    bool _released=false,_claimed=false;
};
// Shared by witnesses, survives late destructors. Logical known C++ metadata
// charge includes an explicit control-block guard, not allocator/RSS proof.
class StandaloneMeshWitnessBudget : public std::enable_shared_from_this<StandaloneMeshWitnessBudget>
{
public:
    struct Stats { size_t live=0,bytes=0,peak=0,refused=0; };
    static constexpr size_t MaxWitnesses=8192,MaxBytes=1024*1024;
    static constexpr size_t Charge=sizeof(StandaloneMeshRetirementWitness)+64;
    std::shared_ptr<StandaloneMeshRetirementWitness> Create(uint64_t producer,const std::shared_ptr<SharedMeshEpoch>& epoch) noexcept
    {
        { std::lock_guard lock(_mutex);
          if(!producer || !epoch || _stats.live>=MaxWitnesses || Charge>MaxBytes-_stats.bytes) { if(_stats.refused!=std::numeric_limits<size_t>::max()) ++_stats.refused; return {}; }
          ++_stats.live; _stats.bytes+=Charge; if(_stats.bytes>_stats.peak) _stats.peak=_stats.bytes; }
        try {
            auto self=shared_from_this();
            // shared_ptr invokes the deleter if control-block allocation fails.
            auto* raw=new StandaloneMeshRetirementWitness(producer,epoch);
            try { return {raw,[self](auto* witness) { delete witness; self->Release(); }}; }
            catch(...) { NoteRefused(); return {}; }
        } catch(...) { Release(); NoteRefused(); return {}; }
    }
    Stats Snapshot() { std::lock_guard lock(_mutex); return _stats; }
private:
    void NoteRefused() { std::lock_guard lock(_mutex); if(_stats.refused!=std::numeric_limits<size_t>::max()) ++_stats.refused; }
    void Release() { std::lock_guard lock(_mutex); --_stats.live; _stats.bytes-=Charge; }
    std::mutex _mutex; Stats _stats;
};
}
