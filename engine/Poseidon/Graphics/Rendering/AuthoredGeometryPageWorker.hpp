#pragma once
#include <Poseidon/Graphics/Rendering/GeometryPageRepresentationDecode.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageDiskSource.hpp>
#include <Poseidon/Graphics/Rendering/GeometryPageHierarchicalDiskSource.hpp>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace Poseidon::GeometryPages
{
// Private authored fixture only. Value-owned source: no Shape/Engine/FFI pointers.
// This input is immutable before Submit and throughout every worker job.
struct AuthoredPageInput
{
    Package package;
    ShapeExport original;
    CacheIdentity identity;
    uint64_t knownCapacityBytes=0;
    static constexpr uint64_t MaxSourceBytes=128*1024;
};
inline uint64_t AuthoredPageKnownBytes(const AuthoredPageInput& input)
{
    uint64_t bytes=sizeof(input)+input.package.pages.capacity()*sizeof(Page)+input.package.clusters.capacity()*sizeof(Cluster);
    for(const auto& page:input.package.pages) bytes+=page.bytes.capacity();
    for(const auto* mesh:{&input.original.coarse,&input.original.fine})
        bytes+=mesh->positions.capacity()*sizeof(Position)+mesh->vertices.capacity()*sizeof(SVertex)+
            (mesh->indices.capacity()+mesh->materials.capacity())*sizeof(uint32_t);
    return bytes;
}
class AuthoredPageWorker
{
public:
    static constexpr uint64_t ResultReservation=128*1024, MetadataReservation=16*1024;
    static constexpr uint64_t MaxReserved=1024*1024;
    static constexpr uint64_t DiskFileReservation=128*1024, DiskSourceReservation=128*1024;
    static constexpr uint64_t HierarchyFileReservation=65536, HierarchyManifestReservation=128*1024;
    enum class SubmitStatus { Queued, Duplicate, Busy, Invalid, Capacity, Failed };
    struct Stats { uint32_t queued=0,active=0,ready=0,liveJobs=0; uint64_t reservedBytes=0,completed=0,cancelled=0,stale=0,failedEpoch=0,failedRequest=0; uint32_t awaitingPublication=0; };
private:
    struct Budget {
        mutable std::mutex mutex;uint32_t live=0;uint64_t bytes=0;
        void Acquire(uint64_t amount) { std::lock_guard lock(mutex);++live;bytes+=amount; }
        void Release(uint64_t amount) { std::lock_guard lock(mutex);bytes-=amount;--live; }
        std::pair<uint32_t,uint64_t> Snapshot() const {std::lock_guard lock(mutex);return {live,bytes};}
    };
    struct Job
    {
        uint64_t epoch,request,reservation;
        std::shared_ptr<const AuthoredPageInput> input;
        std::shared_ptr<const DiskPageInput> disk;
        std::shared_ptr<const HierarchicalDiskPageInput> hierarchyDisk;
        std::shared_ptr<Budget> budget;
        std::atomic<bool> cancelled{false};
        Job(uint64_t e,uint64_t r,std::shared_ptr<const AuthoredPageInput> i,std::shared_ptr<Budget> b)
            :epoch(e),request(r),reservation(i->knownCapacityBytes+ResultReservation+MetadataReservation),input(std::move(i)),budget(std::move(b))
        { budget->Acquire(reservation); }
        Job(uint64_t e,uint64_t r,std::shared_ptr<const DiskPageInput> d,std::shared_ptr<Budget> b)
            :epoch(e),request(r),reservation(DiskPageKnownBytes(*d)+DiskFileReservation+DiskSourceReservation+ResultReservation+MetadataReservation),disk(std::move(d)),budget(std::move(b))
        { budget->Acquire(reservation); }
        Job(uint64_t e,uint64_t r,std::shared_ptr<const HierarchicalDiskPageInput> d,std::shared_ptr<Budget> b)
            :epoch(e),request(r),reservation(HierarchicalDiskPageInputKnownBytes(*d)+HierarchyFileReservation+
                HierarchyManifestReservation+ResultReservation+MetadataReservation),hierarchyDisk(std::move(d)),budget(std::move(b))
        { budget->Acquire(reservation); }
        ~Job() { budget->Release(reservation); }
    };
public:
    struct Result
    {
        uint64_t epoch=0,request=0;
        DecodeStatus status=DecodeStatus::Invalid;
        DiskReadStatus diskStatus=DiskReadStatus::NotRequested;
        Frontier requestedRepresentation=Frontier::Fine;
        ResidentRepresentation decoded;
        std::shared_ptr<const AuthoredPageInput> source;
        // Independent hierarchy jobs share this worker, but return one page
        // rather than a complete authored representation. Zero/default fields
        // leave existing authored and selected-cut result contracts unchanged.
        HierarchicalDiskReadStatus hierarchyDiskStatus=HierarchicalDiskReadStatus::NotRequested;
        uint32_t hierarchyPageId=UINT32_MAX;
        HierarchicalIdentity hierarchyIdentity;
        HierarchicalPage hierarchyPage;
        std::shared_ptr<const HierarchicalDiskPageInput> hierarchySource;
        // Keeps the exact request source/reservation alive through owner staging.
        std::shared_ptr<Job> ownership;
    };
private:
    std::shared_ptr<Budget> budget_=std::make_shared<Budget>();
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool closed_=false,holdBeforePublish_=false,holdBeforeDiskOpen_=false,awaitingPublication_=false;
    std::shared_ptr<Job> pending_,active_;
    std::unique_ptr<Result> ready_;
    uint64_t completed_=0,cancelled_=0,stale_=0,failedEpoch_=0,failedRequest_=0;
    std::thread thread_;
    void Run()
    {
        for(;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock,[&]{return closed_ || (pending_ && !ready_);});
                if(closed_) return;
                job=std::move(pending_);active_=job;
                if(job->disk||job->hierarchyDisk) changed_.wait(lock,[&]{return closed_ || !holdBeforeDiskOpen_;});
            }
            std::unique_ptr<Result> result;
            try {
                result=std::make_unique<Result>(); result->epoch=job->epoch;result->request=job->request;result->ownership=job;result->source=job->input;
                result->requestedRepresentation=job->disk?job->disk->representation:Frontier::Fine;
                if(job->hierarchyDisk) {
                    result->hierarchySource=job->hierarchyDisk;
                    result->hierarchyPageId=job->hierarchyDisk->pageId;
                    result->hierarchyIdentity=job->hierarchyDisk->expectedIdentity;
                    result->hierarchyDiskStatus=ReadHierarchicalDiskPage(*job->hierarchyDisk,result->hierarchyPage,job->cancelled);
                    switch(result->hierarchyDiskStatus) {
                        case HierarchicalDiskReadStatus::Read:result->status=DecodeStatus::Decoded;break;
                        case HierarchicalDiskReadStatus::Cancelled:result->status=DecodeStatus::Cancelled;break;
                        case HierarchicalDiskReadStatus::Capacity:result->status=DecodeStatus::Capacity;break;
                        case HierarchicalDiskReadStatus::AllocationFailed:result->status=DecodeStatus::AllocationFailed;break;
                        default:result->status=DecodeStatus::Invalid;break;
                    }
                }
                if(job->disk) {
                    ClodRamPackage loaded;result->diskStatus=ReadDiskPageSource(*job->disk,loaded,job->cancelled);
                    if(result->diskStatus==DiskReadStatus::Read && !job->cancelled.load()) {
                        auto source=std::make_shared<AuthoredPageInput>();
                        source->package=std::move(loaded.package);source->original=std::move(loaded.selectedGeometry);
                        source->identity=source->package.Identity();source->knownCapacityBytes=AuthoredPageKnownBytes(*source);
                        if(source->knownCapacityBytes>DiskSourceReservation) result->diskStatus=DiskReadStatus::Capacity;
                        else result->source=std::move(source);
                    }
                }
                RepresentationDecodeLimits limits;limits.pages=8;limits.clusters=64;limits.vertexRecords=1024;limits.indices=4096;
                limits.decodedBytes=ResultReservation;limits.serializedBytes=AuthoredPageInput::MaxSourceBytes;
                if(!job->hierarchyDisk && result->source && !job->cancelled.load())
                    result->status=DecodeResidentRepresentation(result->source->package,result->source->identity,result->requestedRepresentation,
                        result->source->original,result->decoded,limits,&job->cancelled);
                else if(job->cancelled.load()) result->status=DecodeStatus::Cancelled;
                uint64_t capacity=sizeof(Result)+result->decoded.pages.capacity()*sizeof(ResidentPage);
                for(const auto& page:result->decoded.pages) {
                    capacity+=page.clusters.capacity()*sizeof(ResidentCluster);
                    for(const auto& cluster:page.clusters) capacity+=cluster.vertices.capacity()*sizeof(SVertex)+cluster.indices.capacity()*sizeof(uint32_t);
                }
                capacity+=HierarchicalDiskPageKnownBytes(result->hierarchyPage)-sizeof(HierarchicalPage);
                if(capacity>ResultReservation) { result->decoded={};result->hierarchyPage={};result->status=DecodeStatus::Capacity;
                    if(job->hierarchyDisk)result->hierarchyDiskStatus=HierarchicalDiskReadStatus::Capacity; }
            } catch(...) { result.reset(); }
            {
                std::unique_lock lock(mutex_);
                // Explicit test/fixture latch; cancellation debt remains until real
                // worker exit from this job. Never enabled by ordinary submission.
                awaitingPublication_=true;
                changed_.wait(lock,[&]{return closed_ || !holdBeforePublish_;});
                awaitingPublication_=false;
                if(closed_ || job->cancelled.load()) {
                    ++cancelled_; lock.unlock();result.reset();lock.lock();
                } else if(result) { ready_=std::move(result);++completed_; }
                else {failedEpoch_=job->epoch;failedRequest_=job->request;}
                active_.reset();
            }
            job.reset(); // reservation released only after result/source ownership exits
            changed_.notify_all();
        }
    }
public:
    AuthoredPageWorker():thread_([this]{Run();}) {}
    ~AuthoredPageWorker()
    {
        std::shared_ptr<Job> pending;std::unique_ptr<Result> ready;
        { std::lock_guard lock(mutex_);closed_=true;holdBeforePublish_=false;holdBeforeDiskOpen_=false;
          if(active_) active_->cancelled.store(true);pending=std::move(pending_);ready=std::move(ready_); }
        changed_.notify_all(); if(thread_.joinable()) thread_.join();
    }
    AuthoredPageWorker(const AuthoredPageWorker&)=delete;
    SubmitStatus Submit(uint64_t epoch,uint64_t request,std::shared_ptr<const AuthoredPageInput> input,uint64_t availableBytes)
    {
        if(!epoch || !request || !input || !(input->package.Identity()==input->identity) || !(input->original.source==input->identity.source) ||
           input->package.pages.size()>8 || input->package.clusters.size()>64 || !input->knownCapacityBytes ||
           input->knownCapacityBytes!=AuthoredPageKnownBytes(*input) || input->knownCapacityBytes>AuthoredPageInput::MaxSourceBytes)
            return SubmitStatus::Invalid;
        const uint64_t reservation=input->knownCapacityBytes+ResultReservation+MetadataReservation;
        std::lock_guard lock(mutex_);
        if(closed_) return SubmitStatus::Failed;
        const auto same=[&](const std::shared_ptr<Job>& job){return job && job->epoch==epoch && job->request==request && !job->cancelled.load();};
        if(same(active_)||same(pending_)||(ready_&&same(ready_->ownership))) return SubmitStatus::Duplicate;
        const auto [live,bytes]=budget_->Snapshot();
        if(pending_ || live>=2) return SubmitStatus::Busy;
        if(availableBytes>MaxReserved || bytes>availableBytes || reservation>availableBytes-bytes) return SubmitStatus::Capacity;
        try { pending_=std::make_shared<Job>(epoch,request,std::move(input),budget_); }
        catch(...) { return SubmitStatus::Failed; }
        changed_.notify_all();return SubmitStatus::Queued;
    }
    SubmitStatus SubmitDisk(uint64_t epoch,uint64_t request,std::shared_ptr<const DiskPageInput> input,uint64_t availableBytes)
    {
        if(!epoch || !request || !input || !ValidDiskPageInput(*input)) return SubmitStatus::Invalid;
        const uint64_t reservation=DiskPageKnownBytes(*input)+DiskFileReservation+DiskSourceReservation+ResultReservation+MetadataReservation;
        std::lock_guard lock(mutex_);
        if(closed_) return SubmitStatus::Failed;
        const auto same=[&](const std::shared_ptr<Job>& job){return job && job->epoch==epoch && job->request==request && !job->cancelled.load();};
        if(same(active_)||same(pending_)||(ready_&&same(ready_->ownership))) return SubmitStatus::Duplicate;
        const auto [live,bytes]=budget_->Snapshot();
        if(pending_ || live>=2) return SubmitStatus::Busy;
        if(availableBytes>MaxReserved || bytes>availableBytes || reservation>availableBytes-bytes) return SubmitStatus::Capacity;
        try {
            // Submission metadata may have mutable aliases in caller code. Copy
            // only after preflight; the Job never keeps a caller path/key alias.
            // Caller must still avoid concurrent mutation DURING SubmitDisk.
            auto frozen=std::make_shared<const DiskPageInput>(*input);
            if(!ValidDiskPageInput(*frozen)) return SubmitStatus::Invalid;
            const uint64_t frozenReservation=DiskPageKnownBytes(*frozen)+DiskFileReservation+DiskSourceReservation+ResultReservation+MetadataReservation;
            if(frozenReservation>availableBytes-bytes) return SubmitStatus::Capacity;
            pending_=std::make_shared<Job>(epoch,request,std::move(frozen),budget_);
        } catch(...) {return SubmitStatus::Failed;}
        changed_.notify_all();return SubmitStatus::Queued;
    }
    SubmitStatus SubmitHierarchyDisk(uint64_t epoch,uint64_t request,std::shared_ptr<const HierarchicalDiskPageInput> input,uint64_t availableBytes)
    {
        if(!epoch||!request||!input||!ValidHierarchicalDiskPageInput(*input))return SubmitStatus::Invalid;
        const uint64_t reservation=HierarchicalDiskPageInputKnownBytes(*input)+HierarchyFileReservation+
            HierarchyManifestReservation+ResultReservation+MetadataReservation;
        std::lock_guard lock(mutex_);
        if(closed_)return SubmitStatus::Failed;
        const auto same=[&](const std::shared_ptr<Job>& job){return job&&job->epoch==epoch&&job->request==request&&!job->cancelled.load();};
        const auto matching=[&](const std::shared_ptr<Job>& job){return job->hierarchyDisk&&
            job->hierarchyDisk->pageId==input->pageId&&job->hierarchyDisk->path==input->path&&
            job->hierarchyDisk->expectedIdentity==input->expectedIdentity&&
            job->hierarchyDisk->expectedMetadataSha256==input->expectedMetadataSha256&&
            job->hierarchyDisk->fileBytes==input->fileBytes&&job->hierarchyDisk->metadataBytes==input->metadataBytes;};
        for(const auto& job:{active_,pending_,ready_?ready_->ownership:std::shared_ptr<Job>{}})
            if(same(job))return matching(job)?SubmitStatus::Duplicate:SubmitStatus::Invalid;
        const auto [live,bytes]=budget_->Snapshot();
        if(pending_||live>=2)return SubmitStatus::Busy;
        if(availableBytes>MaxReserved||bytes>availableBytes||reservation>availableBytes-bytes)return SubmitStatus::Capacity;
        try {
            // Freeze BOTH path and prefix after preflight. A caller must still
            // avoid concurrent mutation DURING submission; later aliases cannot
            // replace the authenticated table or page address of this job.
            auto frozen=std::make_shared<const HierarchicalDiskPageInput>(*input);
            if(!ValidHierarchicalDiskPageInput(*frozen))return SubmitStatus::Invalid;
            const uint64_t charge=HierarchicalDiskPageInputKnownBytes(*frozen)+HierarchyFileReservation+
                HierarchyManifestReservation+ResultReservation+MetadataReservation;
            if(charge>availableBytes-bytes)return SubmitStatus::Capacity;
            pending_=std::make_shared<Job>(epoch,request,std::move(frozen),budget_);
        }catch(...){return SubmitStatus::Failed;}
        changed_.notify_all();return SubmitStatus::Queued;
    }
    // Deterministic test latch; default false. Held cancellation debt belongs to
    // the actual active worker until released/closed, not the request registry.
    void HoldBeforeDiskOpen(bool hold) { {std::lock_guard lock(mutex_);holdBeforeDiskOpen_=hold;}changed_.notify_all(); }
    void Cancel(uint64_t epoch)
    {
        std::shared_ptr<Job> pending;std::unique_ptr<Result> ready;
        { std::lock_guard lock(mutex_);
          if(active_ && active_->epoch==epoch) active_->cancelled.store(true);
          if(pending_ && pending_->epoch==epoch) {pending_->cancelled.store(true);pending=std::move(pending_);++cancelled_;}
          if(ready_ && ready_->epoch==epoch) {ready_->ownership->cancelled.store(true);ready=std::move(ready_);++cancelled_;} }
        changed_.notify_all();
    }
    std::unique_ptr<Result> Take(uint64_t epoch,uint64_t request)
    {
        std::unique_ptr<Result> stale,result;
        { std::lock_guard lock(mutex_);
          if(ready_ && ready_->epoch==epoch && ready_->request==request && !ready_->ownership->cancelled.load()) result=std::move(ready_);
          else if(ready_) {stale=std::move(ready_);++stale_;} }
        changed_.notify_all();return result;
    }
    void HoldBeforePublish(bool hold) { {std::lock_guard lock(mutex_);holdBeforePublish_=hold;}changed_.notify_all(); }
    Stats Snapshot() const
    {
        std::lock_guard lock(mutex_); const auto [live,bytes]=budget_->Snapshot();
        return {uint32_t(bool(pending_)),uint32_t(bool(active_)),uint32_t(bool(ready_)),live,bytes,completed_,cancelled_,stale_,failedEpoch_,failedRequest_,uint32_t(awaitingPublication_)};
    }
};
}
