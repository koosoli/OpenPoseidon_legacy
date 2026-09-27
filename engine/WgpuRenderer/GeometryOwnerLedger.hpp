#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Poseidon::render
{
// Opt-in serial-owner diagnostic only. Numeric producer estimates are neither Rust
// allocation acknowledgements nor fence/pool retirement proof. No reclaim decisions.
class GeometryOwnerLedger
{
public:
    enum class ProducerKind { Retained, Cpu };
    enum class Reason : uint32_t { Protocol=1, Capacity=2, LateEnable=4, Arithmetic=8 };
    struct Limits { size_t allocations=8192, borrowers=32768, modelLodLinks=32768, mailbox=4096; };
    struct Report
    {
        uint64_t epoch=0, incompleteReasons=0;
        bool attributionComplete=false, ownershipCoverageComplete=false;
        uint64_t reclaimableBytes=0;
        size_t trackedAllocations=0, trackedBorrowerRecords=0, liveCpuBorrowers=0, modelLodLinks=0, queuedDetachEvents=0;
        uint64_t droppedDetachEvents=0, cancelledBeforeCreate=0;
        // Producer estimates for records not scheduled-retired by this ledger.
        // Includes detached zero-owner records without retirement acknowledgement;
        // never current GPU-live bytes or physical/free-pool completion proof.
        uint64_t observedProducerEstimatedVertexBytes=0, observedProducerEstimatedIndexBytes=0;
        uint64_t cpuReferencedBytes=0, cpuReferencedVertexBytes=0, cpuReferencedIndexBytes=0;
        uint64_t retainedSharedWithCpuBytes=0, retainedSharedWithCpuVertexBytes=0, retainedSharedWithCpuIndexBytes=0;
        uint64_t standaloneCpuBytes=0, standaloneCpuVertexBytes=0, standaloneCpuIndexBytes=0;
        uint64_t persistentOwnerBytes=0, zeroPersistentOwnerBytes=0;
    };
    GeometryOwnerLedger() : GeometryOwnerLedger(Limits{}) {}
    explicit GeometryOwnerLedger(Limits limits) : _limits(limits) {}
    // BeginEpoch must precede all observed creates. Epochs are monotonic/nonzero;
    // buffer-held shared_ptr lifetime must outlast all numeric destructor callbacks.
    bool BeginEpoch(uint64_t epoch, bool observedFromStart=true)
    {
        if (!epoch || epoch<=_epoch) { if (_open) MarkIncomplete(_epoch,Reason::Protocol); return false; }
        std::lock_guard<std::mutex> lock(_mailMutex);
        _allocations.clear(); _borrowers.clear(); _models.clear(); _mail.clear();
        _epoch=epoch; _mailEpoch=epoch; _open=true; _mailOpen=true;
        _reasons=observedFromStart?0:static_cast<uint32_t>(Reason::LateEnable);
        _mailDropped=0; _drops=0; _cancelled=0; _links=0;
        return true;
    }
    void CloseEpoch(uint64_t epoch)
    {
        if (!Current(epoch)) return;
        DrainDetachMailbox();
        std::lock_guard<std::mutex> lock(_mailMutex);
        _mailOpen=false; _mail.clear(); _open=false;
    }
    void MarkIncomplete(uint64_t epoch, Reason reason) { if(Current(epoch)) _reasons|=static_cast<uint32_t>(reason); }
    bool AllocationCreated(uint64_t epoch,uint64_t id,uint64_t vertexBytes,uint64_t indexBytes,ProducerKind kind,uint64_t meshHandle=0)
    {
        if(!Current(epoch)) return false;
        if(!id || _allocations.count(id)) { MarkIncomplete(epoch,Reason::Protocol); return false; }
        if(_allocations.size()>=_limits.allocations) { MarkIncomplete(epoch,Reason::Capacity); return false; }
        try { _allocations.emplace(id,Allocation{vertexBytes,indexBytes,meshHandle,kind,true,false,0,0}); }
        catch (...) { MarkIncomplete(epoch,Reason::Capacity); return false; }
        if(!meshHandle) MarkIncomplete(epoch,Reason::Protocol);
        return true;
    }
    bool MeshHandleAssigned(uint64_t epoch,uint64_t id,uint64_t handle)
    {
        auto* a=Find(epoch,id); if(!a) return false;
        if(!handle || (a->handle && a->handle!=handle)) { MarkIncomplete(epoch,Reason::Protocol); return false; }
        a->handle=handle; return true;
    }
    bool CpuBorrowerAttached(uint64_t epoch,uint64_t borrower,uint64_t allocation)
    {
        if(!Current(epoch)) return false;
        DrainDetachMailbox();
        auto* a=Find(epoch,allocation); if(!a || !borrower || a->retired) { MarkIncomplete(epoch,Reason::Protocol); return false; }
        auto found=_borrowers.find(borrower);
        if(found!=_borrowers.end()) {
            if(!found->second.attached) return false; // cancellation tombstone
            if(found->second.allocation==allocation) return true;
            MarkIncomplete(epoch,Reason::Protocol); return false;
        }
        if(_borrowers.size()>=_limits.borrowers) { MarkIncomplete(epoch,Reason::Capacity); return false; }
        try { _borrowers.emplace(borrower,Borrower{allocation,true}); }
        catch (...) { MarkIncomplete(epoch,Reason::Capacity); return false; }
        ++a->cpu; return true;
    }
    void CpuBorrowerDetached(uint64_t epoch,uint64_t borrower)
    {
        if(!Current(epoch) || !borrower) return;
        auto found=_borrowers.find(borrower);
        if(found==_borrowers.end()) {
            if(_borrowers.size()>=_limits.borrowers) { MarkIncomplete(epoch,Reason::Capacity); return; }
            try { _borrowers.emplace(borrower,Borrower{0,false}); }
            catch (...) { MarkIncomplete(epoch,Reason::Capacity); }
            return;
        }
        if(found->second.attached) {
            auto a=_allocations.find(found->second.allocation);
            if(a!=_allocations.end() && a->second.cpu) --a->second.cpu;
            else MarkIncomplete(epoch,Reason::Protocol);
            found->second.attached=false;
        }
    }
    // Only method callable from arbitrary destructor threads. No renderer/slot reads.
    void QueueCpuBorrowerDetached(uint64_t epoch,uint64_t borrower) noexcept
    {
        std::lock_guard<std::mutex> lock(_mailMutex);
        if(!_mailOpen || epoch!=_mailEpoch || !borrower) return;
        if(_mail.size()>=_limits.mailbox) { if(_mailDropped!=std::numeric_limits<uint64_t>::max()) ++_mailDropped; return; }
        try { _mail.push_back(borrower); } catch (...) { if(_mailDropped!=std::numeric_limits<uint64_t>::max()) ++_mailDropped; }
    }
    void DrainDetachMailbox()
    {
        std::vector<uint64_t> events;
        uint64_t dropped=0;
        { std::lock_guard<std::mutex> lock(_mailMutex); events.swap(_mail); dropped=_mailDropped; _mailDropped=0; }
        if(!_open) return;
        if(dropped) { Add(_drops,dropped); MarkIncomplete(_epoch,Reason::Capacity); }
        for(auto borrower:events) CpuBorrowerDetached(_epoch,borrower);
    }
    bool ModelLodAttached(uint64_t epoch,uint32_t model,uint32_t lod,uint64_t allocation)
    {
        auto* a=Find(epoch,allocation); if(!a) return false;
        if(a->retired) { MarkIncomplete(epoch,Reason::Protocol); return false; }
        auto m=_models.find(model);
        if(m!=_models.end()) {
            auto existing=m->second.find(lod);
            if(existing!=m->second.end() && existing->second.count(allocation)) return true;
        }
        if(_links>=_limits.modelLodLinks) { MarkIncomplete(epoch,Reason::Capacity); return false; }
        try { _models[model][lod].insert(allocation); }
        catch (...) {
            auto partial=_models.find(model);
            if(partial!=_models.end()) {
                auto empty=partial->second.find(lod);
                if(empty!=partial->second.end() && empty->second.empty()) partial->second.erase(empty);
                if(partial->second.empty()) _models.erase(partial);
            }
            MarkIncomplete(epoch,Reason::Capacity); return false;
        }
        ++_links; ++a->models; return true;
    }
    void ModelRetired(uint64_t epoch,uint32_t model)
    {
        if(!Current(epoch)) return;
        auto m=_models.find(model); if(m==_models.end()) return;
        for(const auto& lod:m->second) for(auto allocation:lod.second) {
            auto a=_allocations.find(allocation);
            if(a!=_allocations.end() && a->second.models) --a->second.models;
            else MarkIncomplete(epoch,Reason::Protocol);
            --_links;
        }
        _models.erase(m);
    }
    void ProducerOwnershipReleased(uint64_t epoch,uint64_t id) { auto* a=Find(epoch,id); if(a) a->producer=false; }
    // Records a scheduled producer retirement only; no physical GPU completion claim.
    void AllocationRetired(uint64_t epoch,uint64_t id)
    {
        if(!Current(epoch)) return;
        DrainDetachMailbox();
        auto* a=Find(epoch,id); if(!a) return;
        if(a->cpu || a->models) MarkIncomplete(epoch,Reason::Protocol);
        a->producer=false; a->retired=true;
    }
    void NoteCancelledBeforeCreate(uint64_t epoch) { if(Current(epoch)) Add(_cancelled,1); }
    Report Snapshot()
    {
        DrainDetachMailbox(); Report r; r.epoch=_epoch; r.trackedAllocations=_allocations.size();
        r.trackedBorrowerRecords=_borrowers.size(); r.modelLodLinks=_links; r.droppedDetachEvents=_drops; r.cancelledBeforeCreate=_cancelled;
        { std::lock_guard<std::mutex> lock(_mailMutex); r.queuedDetachEvents=_mail.size(); }
        for(const auto& pair:_allocations) {
            const auto& a=pair.second; r.liveCpuBorrowers+=a.cpu;
            if(a.retired) continue;
            Add(r.observedProducerEstimatedVertexBytes,a.vertex); Add(r.observedProducerEstimatedIndexBytes,a.index);
            uint64_t bytes=0; Add(bytes,a.vertex); Add(bytes,a.index);
            if(a.cpu) { Add(r.cpuReferencedBytes,bytes); Add(r.cpuReferencedVertexBytes,a.vertex); Add(r.cpuReferencedIndexBytes,a.index); }
            if(a.kind==ProducerKind::Retained && a.cpu && a.models) {
                Add(r.retainedSharedWithCpuBytes,bytes); Add(r.retainedSharedWithCpuVertexBytes,a.vertex); Add(r.retainedSharedWithCpuIndexBytes,a.index);
            }
            if(a.kind==ProducerKind::Cpu && a.cpu) { Add(r.standaloneCpuBytes,bytes); Add(r.standaloneCpuVertexBytes,a.vertex); Add(r.standaloneCpuIndexBytes,a.index); }
            if(a.producer || a.models || a.cpu) Add(r.persistentOwnerBytes,bytes); else Add(r.zeroPersistentOwnerBytes,bytes);
        }
        r.incompleteReasons=_reasons; r.attributionComplete=_open && !_reasons && r.queuedDetachEvents==0; return r;
    }
private:
    struct Allocation { uint64_t vertex,index,handle; ProducerKind kind; bool producer,retired; size_t cpu,models; };
    struct Borrower { uint64_t allocation; bool attached; };
    bool Current(uint64_t epoch) const { return _open && epoch==_epoch; }
    Allocation* Find(uint64_t epoch,uint64_t id)
    {
        if(!Current(epoch)) return nullptr;
        auto a=_allocations.find(id); if(a==_allocations.end()) { MarkIncomplete(epoch,Reason::Protocol); return nullptr; }
        return &a->second;
    }
    void Add(uint64_t& sum,uint64_t value)
    {
        if(value>std::numeric_limits<uint64_t>::max()-sum) { sum=std::numeric_limits<uint64_t>::max(); MarkIncomplete(_epoch,Reason::Arithmetic); }
        else sum+=value;
    }
    const Limits _limits;
    uint64_t _epoch=0,_reasons=0,_drops=0,_cancelled=0;
    bool _open=false;
    size_t _links=0;
    std::unordered_map<uint64_t,Allocation> _allocations;
    std::unordered_map<uint64_t,Borrower> _borrowers;
    std::unordered_map<uint32_t,std::unordered_map<uint32_t,std::unordered_set<uint64_t>>> _models;
    std::mutex _mailMutex;
    uint64_t _mailEpoch=0,_mailDropped=0;
    bool _mailOpen=false;
    std::vector<uint64_t> _mail;
};
}
