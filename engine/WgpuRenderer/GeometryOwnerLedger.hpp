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
    enum class Reason : uint32_t { Protocol=1, Capacity=2, LateEnable=4, Arithmetic=8, HandleExportCapacity=16 };
    struct Limits { size_t allocations=8192, borrowers=32768, modelLodLinks=32768, mailbox=4096; };
    struct Report
    {
        uint64_t epoch=0, incompleteReasons=0;
        bool attributionComplete=false, ownershipCoverageComplete=false;
        uint64_t reclaimableBytes=0;
        size_t trackedAllocations=0, trackedBorrowerRecords=0, liveCpuBorrowers=0, modelLodLinks=0, queuedDetachEvents=0;
        // Legacy event counters include both numeric detach and scheduling facts.
        uint64_t droppedDetachEvents=0, cancelledBeforeCreate=0;
        uint64_t mailboxKnownCapacityBytes=0, mailboxPeakKnownCapacityBytes=0;
        uint64_t scheduledRetirementEvents=0;
        size_t queuedOwnershipEvents=0, queuedRetirementEvents=0;
        uint64_t droppedOwnershipEvents=0, droppedRetirementEvents=0;
        // Producer estimates for records not scheduled-retired by this ledger.
        // Includes detached zero-owner records without retirement acknowledgement;
        // never current GPU-live bytes or physical/free-pool completion proof.
        uint64_t observedProducerEstimatedVertexBytes=0, observedProducerEstimatedIndexBytes=0;
        uint64_t cpuReferencedBytes=0, cpuReferencedVertexBytes=0, cpuReferencedIndexBytes=0;
        uint64_t retainedSharedWithCpuBytes=0, retainedSharedWithCpuVertexBytes=0, retainedSharedWithCpuIndexBytes=0;
        uint64_t standaloneCpuBytes=0, standaloneCpuVertexBytes=0, standaloneCpuIndexBytes=0;
        uint64_t persistentOwnerBytes=0, zeroPersistentOwnerBytes=0;
    };
    struct HandleRecord
    {
        uint64_t allocationId=0, rendererHandle=0, epoch=0;
        bool scheduledRetired=false, producerOwned=false;
        uint64_t cpuBorrowers=0, modelLodLinks=0;
    };
    struct HandleReport
    {
        uint64_t epoch=0,incompleteReasons=0;
        bool attributionComplete=false,truncated=false;
        size_t queuedDetachEvents=0,trackedAllocations=0;
        size_t queuedOwnershipEvents=0; // truthful total; queuedDetachEvents is the historical all-event alias
        Report ownerReport; // Same map cut as records, without a second mailbox drain.
        std::vector<HandleRecord> records;
    };
    // Serial owner-only selected-record cut. Drains the bounded numeric mailbox,
    // then copies one O(1) map lookup; missing evidence is explicit, never Ready.
    bool SnapshotHandle(uint64_t epoch,uint64_t allocation,HandleRecord& result)
    {
        result={};
        if(!Current(epoch)) return false;
        DrainDetachMailbox();
        const auto found=_allocations.find(allocation);
        if(found==_allocations.end()) return false;
        const auto& a=found->second;
        result=HandleRecord{allocation,a.handle,_epoch,a.retired,a.producer,
            static_cast<uint64_t>(a.cpu),static_cast<uint64_t>(a.models)};
        return true;
    }
    static constexpr size_t MaxHandleExportRecords=8192;
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
        _mailDropped=0; _drops=0; _cancelled=0; _links=0; _scheduledEvents=0; _mailPeakBytes=_mail.capacity()*sizeof(MailEvent); _mailScheduledQueued=0; _mailScheduledDropped=0; _retireDrops=0;
        return true;
    }
    void CloseEpoch(uint64_t epoch)
    {
        if (!Current(epoch)) return;
        DrainDetachMailbox();
        std::lock_guard<std::mutex> lock(_mailMutex);
        _mailOpen=false; _mail.clear(); _mailScheduledQueued=0; _open=false;
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
    // Arbitrary destructor threads enqueue immutable numeric metadata only.
    // Scheduling records are emitted only after a real destroy-dispatch callback,
    // not when a helper merely claims a handle. Neither kind acknowledges GPU work.
    void QueueCpuBorrowerDetached(uint64_t epoch,uint64_t borrower) noexcept
    { QueueEvent(epoch,borrower,MailKind::Detach); }
    void QueueAllocationRetirementScheduled(uint64_t epoch,uint64_t allocation) noexcept
    { QueueEvent(epoch,allocation,MailKind::ScheduledRetire); }
    void DrainDetachMailbox()
    {
        std::vector<MailEvent> events;
        uint64_t dropped=0,retirementDrops=0,epoch=0;
        { std::lock_guard<std::mutex> lock(_mailMutex); events.swap(_mail); dropped=_mailDropped; retirementDrops=_mailScheduledDropped;
          _mailDropped=0; _mailScheduledDropped=0; _mailScheduledQueued=0; epoch=_mailEpoch; }
        if(!Current(epoch)) return;
        if(dropped) { Add(_drops,dropped); Add(_retireDrops,retirementDrops); MarkIncomplete(epoch,Reason::Capacity); }
        for(const auto& event:events)
        {
            if(event.kind==MailKind::Detach) CpuBorrowerDetached(epoch,event.id);
            else { ApplyRetirement(epoch,event.id); Add(_scheduledEvents,1); }
        }
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
        ApplyRetirement(epoch,id);
    }
    void NoteCancelledBeforeCreate(uint64_t epoch) { if(Current(epoch)) Add(_cancelled,1); }
    Report Snapshot() { DrainDetachMailbox(); return BuildSnapshot(); }
    // Owner-only nonyielding numeric cut. Every record is copied, including CPU
    // standalone, zero-owner and scheduled-retired records, so an external Rust
    // presence probe can distinguish absence from unknown unobserved handles.
    // This creates no strong resource ownership and never infers retirement.
    // Returned vector allocation is opt-in/on-demand only, at most 8192 records.
    HandleReport SnapshotHandles()
    {
        DrainDetachMailbox();
        HandleReport r; r.epoch=_epoch; r.trackedAllocations=_allocations.size();
        const size_t count=_allocations.size()<MaxHandleExportRecords?_allocations.size():MaxHandleExportRecords;
        if(_allocations.size()>MaxHandleExportRecords) {
            r.truncated=true; r.incompleteReasons|=static_cast<uint32_t>(Reason::HandleExportCapacity); MarkIncomplete(_epoch,Reason::HandleExportCapacity);
        }
        try {
            r.records.reserve(count);
            for(const auto& pair:_allocations) {
                if(r.records.size()>=count) break;
                const auto& a=pair.second;
                r.records.push_back(HandleRecord{pair.first,a.handle,_epoch,a.retired,a.producer,
                    static_cast<uint64_t>(a.cpu),static_cast<uint64_t>(a.models)});
            }
        } catch (...) {
            r.records.clear(); r.truncated=true; r.incompleteReasons|=static_cast<uint32_t>(Reason::Capacity); MarkIncomplete(_epoch,Reason::Capacity);
        }
        r.ownerReport=BuildSnapshot(); // No drain: records and aggregate see identical owner maps.
        r.incompleteReasons|=r.ownerReport.incompleteReasons;
        r.queuedDetachEvents=r.ownerReport.queuedDetachEvents;
        r.queuedOwnershipEvents=r.ownerReport.queuedOwnershipEvents;
        r.attributionComplete=r.ownerReport.attributionComplete && !r.incompleteReasons && !r.truncated;
        r.ownerReport.incompleteReasons=r.incompleteReasons;
        r.ownerReport.attributionComplete=r.attributionComplete;
        return r;
    }
private:
    Report BuildSnapshot()
    {
        Report r; r.epoch=_epoch; r.trackedAllocations=_allocations.size();
        r.trackedBorrowerRecords=_borrowers.size(); r.modelLodLinks=_links; r.droppedDetachEvents=_drops; r.cancelledBeforeCreate=_cancelled;

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
        { std::lock_guard<std::mutex> lock(_mailMutex); r.queuedDetachEvents=_mail.size();
          r.queuedOwnershipEvents=_mail.size(); r.queuedRetirementEvents=_mailScheduledQueued;
          r.mailboxKnownCapacityBytes=_mail.capacity()*sizeof(MailEvent); r.mailboxPeakKnownCapacityBytes=_mailPeakBytes;
          if(_mailDropped) MarkIncomplete(_epoch,Reason::Capacity); }
        r.scheduledRetirementEvents=_scheduledEvents; r.droppedOwnershipEvents=_drops; r.droppedRetirementEvents=_retireDrops;
        r.incompleteReasons=_reasons; r.attributionComplete=_open && !_reasons && r.queuedDetachEvents==0; return r;
    }

    enum class MailKind : uint8_t { Detach, ScheduledRetire };
    struct MailEvent { uint64_t id; MailKind kind; };
    // Vector capacity is actual known C++ payload metadata, not allocator/RSS.
    // Owner drain may transiently hold one bounded batch while producers fill
    // another; neither batch contains slot, renderer or resource pointers.
    void QueueEvent(uint64_t epoch,uint64_t id,MailKind kind) noexcept
    {
        std::lock_guard<std::mutex> lock(_mailMutex);
        if(!_mailOpen || epoch!=_mailEpoch || !id) return;
        if(_mail.size()>=_limits.mailbox) { NoteMailboxDrop(kind); return; }
        try {
            _mail.push_back(MailEvent{id,kind});
            if(kind==MailKind::ScheduledRetire) ++_mailScheduledQueued;
            const auto bytes=_mail.capacity()*sizeof(MailEvent);
            if(bytes>_mailPeakBytes) _mailPeakBytes=bytes;
        } catch (...) { NoteMailboxDrop(kind); }
    }
    void NoteMailboxDrop(MailKind kind)
    {
        if(_mailDropped!=std::numeric_limits<uint64_t>::max()) ++_mailDropped;
        if(kind==MailKind::ScheduledRetire && _mailScheduledDropped!=std::numeric_limits<uint64_t>::max()) ++_mailScheduledDropped;
    }
    void ApplyRetirement(uint64_t epoch,uint64_t id)
    {
        auto* a=Find(epoch,id); if(!a) return;
        if(a->cpu || a->models) MarkIncomplete(epoch,Reason::Protocol);
        a->producer=false; a->retired=true;
    }
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
    uint64_t _epoch=0,_reasons=0,_drops=0,_cancelled=0,_scheduledEvents=0,_retireDrops=0;
    bool _open=false;
    size_t _links=0;
    std::unordered_map<uint64_t,Allocation> _allocations;
    std::unordered_map<uint64_t,Borrower> _borrowers;
    std::unordered_map<uint32_t,std::unordered_map<uint32_t,std::unordered_set<uint64_t>>> _models;
    std::mutex _mailMutex;
    uint64_t _mailEpoch=0,_mailDropped=0,_mailPeakBytes=0,_mailScheduledDropped=0;
    size_t _mailScheduledQueued=0;
    bool _mailOpen=false;
    std::vector<MailEvent> _mail;
};
}
