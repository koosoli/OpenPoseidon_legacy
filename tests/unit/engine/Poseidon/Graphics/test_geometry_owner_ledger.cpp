#include <catch2/catch_test_macros.hpp>
#include <WgpuRenderer/GeometryOwnerLedger.hpp>
#include <WgpuRenderer/MeshRecordClosure.hpp>
#include <WgpuRenderer/SharedRetainedMeshLifetime.hpp>
#include <array>
#include <algorithm>
#include <limits>
#include <thread>
using Poseidon::render::GeometryOwnerLedger;
using Kind=GeometryOwnerLedger::ProducerKind;
using Reason=GeometryOwnerLedger::Reason;

TEST_CASE("Geometry owner ledger unions shared persistent estimates", "[wgpu][geometry-owner-ledger]")
{
    GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(1));
    REQUIRE(l.AllocationCreated(1,10,100,20,Kind::Retained,81));
    REQUIRE(l.ModelLodAttached(1,7,0,10)); REQUIRE(l.ModelLodAttached(1,7,1,10));
    REQUIRE(l.CpuBorrowerAttached(1,1,10)); REQUIRE(l.CpuBorrowerAttached(1,2,10));
    auto r=l.Snapshot(); CHECK(r.liveCpuBorrowers==2); CHECK(r.cpuReferencedBytes==120);
    CHECK(r.retainedSharedWithCpuVertexBytes==100); CHECK(r.retainedSharedWithCpuIndexBytes==20);
    CHECK(r.modelLodLinks==2); CHECK(r.persistentOwnerBytes==120); CHECK(r.attributionComplete);
    CHECK_FALSE(r.ownershipCoverageComplete); CHECK(r.reclaimableBytes==0);
    l.QueueCpuBorrowerDetached(1,2); r=l.Snapshot(); CHECK(r.liveCpuBorrowers==1); CHECK(r.cpuReferencedBytes==120);
    l.ModelRetired(1,7); CHECK(l.Snapshot().retainedSharedWithCpuBytes==0);
    l.ProducerOwnershipReleased(1,10); CHECK(l.Snapshot().persistentOwnerBytes==120);
    l.QueueCpuBorrowerDetached(1,1); r=l.Snapshot(); CHECK(r.persistentOwnerBytes==0); CHECK(r.zeroPersistentOwnerBytes==120);
    CHECK(r.observedProducerEstimatedVertexBytes==100); // detach cannot prove physical retirement
    l.AllocationRetired(1,10); CHECK(l.Snapshot().observedProducerEstimatedVertexBytes==0);
}

TEST_CASE("Geometry owner cancellation tombstone prevents delayed attachment", "[wgpu][geometry-owner-ledger]")
{
    GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(1));
    std::thread destructor([&]{l.QueueCpuBorrowerDetached(1,9);}); destructor.join();
    l.NoteCancelledBeforeCreate(1); REQUIRE(l.AllocationCreated(1,1,70,30,Kind::Cpu,4));
    CHECK_FALSE(l.CpuBorrowerAttached(1,9,1)); CHECK(l.Snapshot().liveCpuBorrowers==0);
    CHECK(l.Snapshot().attributionComplete); CHECK(l.Snapshot().cancelledBeforeCreate==1);
    REQUIRE(l.CpuBorrowerAttached(1,10,1)); l.ProducerOwnershipReleased(1,1);
    CHECK(l.Snapshot().standaloneCpuBytes==100);
    l.CpuBorrowerDetached(1,10); l.CpuBorrowerDetached(1,10);
    CHECK(l.Snapshot().standaloneCpuBytes==0); CHECK(l.Snapshot().zeroPersistentOwnerBytes==100);
}

TEST_CASE("Geometry owner epoch and shutdown reject late numeric callbacks", "[wgpu][geometry-owner-ledger]")
{
    GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(4));
    l.QueueCpuBorrowerDetached(4,1); REQUIRE(l.BeginEpoch(5));
    l.QueueCpuBorrowerDetached(4,2); l.CpuBorrowerDetached(4,3);
    REQUIRE(l.AllocationCreated(5,1,5,7,Kind::Cpu,90)); REQUIRE(l.CpuBorrowerAttached(5,2,1));
    auto r=l.Snapshot(); CHECK(r.trackedBorrowerRecords==1); CHECK(r.liveCpuBorrowers==1); CHECK(r.attributionComplete);
    l.CloseEpoch(5); l.QueueCpuBorrowerDetached(5,2); r=l.Snapshot(); CHECK(r.liveCpuBorrowers==1); CHECK(r.queuedDetachEvents==0);
    CHECK_FALSE(r.attributionComplete); REQUIRE(l.BeginEpoch(6)); CHECK(l.Snapshot().trackedAllocations==0);
    CHECK_FALSE(l.BeginEpoch(6)); CHECK_FALSE(l.Snapshot().attributionComplete);
}

TEST_CASE("Geometry owner caps and protocol failures remain honest", "[wgpu][geometry-owner-ledger]")
{
    SECTION("Late enable") { GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(1,false)); CHECK_FALSE(l.Snapshot().attributionComplete); }
    SECTION("Allocation records retained bounded until reset") {
        GeometryOwnerLedger l({1,2,2,2}); REQUIRE(l.BeginEpoch(1)); REQUIRE(l.AllocationCreated(1,1,1,2,Kind::Retained,90));
        l.AllocationRetired(1,1); CHECK_FALSE(l.AllocationCreated(1,2,1,2,Kind::Cpu,90)); CHECK_FALSE(l.Snapshot().attributionComplete);
        CHECK(l.Snapshot().trackedAllocations==1);
    }
    SECTION("Mailbox overflow") {
        GeometryOwnerLedger l({2,2,2,1}); REQUIRE(l.BeginEpoch(1)); l.QueueCpuBorrowerDetached(1,1); l.QueueCpuBorrowerDetached(1,2);
        auto r=l.Snapshot(); CHECK(r.droppedDetachEvents==1); CHECK_FALSE(r.attributionComplete); CHECK(r.trackedBorrowerRecords==1);
    }
    SECTION("Borrower tombstone cap") {
        GeometryOwnerLedger l({2,1,2,2}); REQUIRE(l.BeginEpoch(1)); l.CpuBorrowerDetached(1,1); l.CpuBorrowerDetached(1,2);
        CHECK(l.Snapshot().trackedBorrowerRecords==1); CHECK_FALSE(l.Snapshot().attributionComplete);
    }
    SECTION("Model link cap") {
        GeometryOwnerLedger l({2,2,1,2}); REQUIRE(l.BeginEpoch(1)); REQUIRE(l.AllocationCreated(1,1,1,2,Kind::Retained,90));
        REQUIRE(l.ModelLodAttached(1,7,0,1)); CHECK_FALSE(l.ModelLodAttached(1,7,1,1)); CHECK(l.Snapshot().modelLodLinks==1);
    }
    SECTION("Scheduled retirement with older borrower is incomplete") {
        GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(1)); REQUIRE(l.AllocationCreated(1,1,5,7,Kind::Retained,90));
        REQUIRE(l.CpuBorrowerAttached(1,1,1)); REQUIRE(l.CpuBorrowerAttached(1,2,1)); l.CpuBorrowerDetached(1,2);
        l.AllocationRetired(1,1); auto r=l.Snapshot(); CHECK(r.liveCpuBorrowers==1); CHECK_FALSE(r.attributionComplete);
        CHECK((r.incompleteReasons & static_cast<uint32_t>(Reason::Protocol))!=0);
    }
    SECTION("Immutable allocation identity and multiple meshes per LOD") {
        GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(1)); REQUIRE(l.AllocationCreated(1,1,5,7,Kind::Retained,8));
        CHECK_FALSE(l.AllocationCreated(1,1,99,7,Kind::Cpu,90)); CHECK_FALSE(l.MeshHandleAssigned(1,1,9));
        REQUIRE(l.AllocationCreated(1,2,3,4,Kind::Retained,90)); REQUIRE(l.ModelLodAttached(1,1,0,1));
        REQUIRE(l.ModelLodAttached(1,1,0,2)); REQUIRE(l.ModelLodAttached(1,1,0,2)); CHECK(l.Snapshot().modelLodLinks==2); CHECK(l.Snapshot().observedProducerEstimatedVertexBytes==8);
    }
    SECTION("Sum overflow saturates and cannot claim exact attribution") {
        GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(1)); REQUIRE(l.AllocationCreated(1,1,std::numeric_limits<uint64_t>::max(),1,Kind::Cpu,90));
        auto r=l.Snapshot(); CHECK(r.persistentOwnerBytes==std::numeric_limits<uint64_t>::max()); CHECK_FALSE(r.attributionComplete);
    }
}

TEST_CASE("Cancelled geometry create drains detach without inventing allocation", "[wgpu][geometry-owner-ledger]")
{
    GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(1));
    l.QueueCpuBorrowerDetached(1,8); l.DrainDetachMailbox(); l.NoteCancelledBeforeCreate(1);
    auto r=l.Snapshot(); CHECK(r.trackedAllocations==0); CHECK(r.liveCpuBorrowers==0);
    CHECK(r.trackedBorrowerRecords==1); CHECK(r.cancelledBeforeCreate==1); CHECK(r.attributionComplete);
    REQUIRE(l.AllocationCreated(1,1,4,2,Kind::Cpu,9)); CHECK_FALSE(l.CpuBorrowerAttached(1,8,1));
    CHECK(l.Snapshot().attributionComplete);
}

TEST_CASE("One model LOD can own multiple shared mesh allocations", "[wgpu][geometry-owner-ledger]")
{
    GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(1));
    REQUIRE(l.AllocationCreated(1,1,10,2,Kind::Retained,91));
    REQUIRE(l.AllocationCreated(1,2,20,4,Kind::Retained,92));
    REQUIRE(l.ModelLodAttached(1,7,0,1)); REQUIRE(l.ModelLodAttached(1,7,0,2));
    REQUIRE(l.ModelLodAttached(1,7,0,1)); REQUIRE(l.CpuBorrowerAttached(1,8,1)); REQUIRE(l.CpuBorrowerAttached(1,9,2));
    auto r=l.Snapshot(); CHECK(r.modelLodLinks==2); CHECK(r.retainedSharedWithCpuBytes==36);
    l.ModelRetired(1,7); r=l.Snapshot(); CHECK(r.modelLodLinks==0); CHECK(r.retainedSharedWithCpuBytes==0);
    CHECK(r.cpuReferencedBytes==36); CHECK(r.attributionComplete);
    l.QueueCpuBorrowerDetached(1,8); l.ProducerOwnershipReleased(1,1); l.AllocationRetired(1,1);
    CHECK(l.Snapshot().attributionComplete); // retirement first drains already queued detach
}

TEST_CASE("Missing renderer handle refuses complete geometry attribution", "[wgpu][geometry-owner-ledger]")
{
    GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(1));
    REQUIRE(l.AllocationCreated(1,1,4,2,Kind::Cpu));
    CHECK_FALSE(l.Snapshot().attributionComplete);
    REQUIRE(l.MeshHandleAssigned(1,1,7)); CHECK_FALSE(l.Snapshot().attributionComplete); // sticky initial gap
}

TEST_CASE("A new model owner cannot silently join a retired producer", "[wgpu][geometry-owner-ledger]")
{
    GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(1));
    REQUIRE(l.AllocationCreated(1,1,10,2,Kind::Retained,8));
    l.AllocationRetired(1,1); REQUIRE(l.Snapshot().attributionComplete);
    CHECK_FALSE(l.ModelLodAttached(1,7,0,1));
    auto r=l.Snapshot(); CHECK(r.modelLodLinks==0); CHECK_FALSE(r.attributionComplete);
    CHECK((r.incompleteReasons & static_cast<uint32_t>(Reason::Protocol))!=0);
}

TEST_CASE("Geometry handle export includes every numeric producer state", "[wgpu][geometry-owner-ledger]")
{
    GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(3));
    REQUIRE(l.AllocationCreated(3,11,10,2,Kind::Retained,91));
    REQUIRE(l.CpuBorrowerAttached(3,1,11)); REQUIRE(l.CpuBorrowerAttached(3,2,11));
    REQUIRE(l.ModelLodAttached(3,7,0,11)); REQUIRE(l.ModelLodAttached(3,7,1,11));
    REQUIRE(l.AllocationCreated(3,12,8,1,Kind::Cpu,92));
    REQUIRE(l.CpuBorrowerAttached(3,3,12)); l.ProducerOwnershipReleased(3,12);
    REQUIRE(l.AllocationCreated(3,13,5,1,Kind::Cpu,93)); l.ProducerOwnershipReleased(3,13);
    REQUIRE(l.AllocationCreated(3,14,5,1,Kind::Retained,94)); l.AllocationRetired(3,14);
    l.QueueCpuBorrowerDetached(3,2);
    const auto captured=l.SnapshotHandles(); REQUIRE(captured.attributionComplete); CHECK_FALSE(captured.truncated);
    CHECK(captured.epoch==3); CHECK(captured.ownerReport.epoch==captured.epoch); CHECK(captured.ownerReport.liveCpuBorrowers==2); CHECK(captured.ownerReport.modelLodLinks==2); CHECK(captured.ownerReport.trackedAllocations==4); CHECK(captured.ownerReport.attributionComplete==captured.attributionComplete); REQUIRE(captured.records.size()==4); CHECK(captured.trackedAllocations==4); CHECK(captured.queuedDetachEvents==0);
    for(const auto& row:captured.records) {
        CHECK(row.epoch==3); CHECK(row.rendererHandle==row.allocationId+80);
        if(row.allocationId==11) { CHECK(row.cpuBorrowers==1); CHECK(row.modelLodLinks==2); CHECK(row.producerOwned); CHECK_FALSE(row.scheduledRetired); }
        if(row.allocationId==12) { CHECK(row.cpuBorrowers==1); CHECK_FALSE(row.producerOwned); CHECK_FALSE(row.scheduledRetired); }
        if(row.allocationId==13) { CHECK(row.cpuBorrowers==0); CHECK(row.modelLodLinks==0); CHECK_FALSE(row.producerOwned); CHECK_FALSE(row.scheduledRetired); }
        if(row.allocationId==14) { CHECK(row.scheduledRetired); CHECK_FALSE(row.producerOwned); }
    }
    // Copied evidence is a value: later owner events cannot mutate an earlier cut.
    l.CpuBorrowerDetached(3,1); l.ModelRetired(3,7); REQUIRE(l.BeginEpoch(4));
    const auto fresh=l.SnapshotHandles(); CHECK(fresh.records.empty()); CHECK(fresh.epoch==4);
    CHECK(captured.records.size()==4); CHECK(captured.epoch==3);
    l.QueueCpuBorrowerDetached(3,1); CHECK(l.SnapshotHandles().attributionComplete);
}

TEST_CASE("Geometry handle export bound refuses complete partial evidence", "[wgpu][geometry-owner-ledger]")
{
    GeometryOwnerLedger l({8193,1,1,1}); REQUIRE(l.BeginEpoch(1));
    for(uint64_t id=1;id<=8193;++id) REQUIRE(l.AllocationCreated(1,id,1,1,Kind::Cpu,id+1));
    const auto report=l.SnapshotHandles(); CHECK(report.trackedAllocations==8193); CHECK(report.records.size()==8192);
    CHECK(report.truncated); CHECK_FALSE(report.attributionComplete); CHECK_FALSE(report.ownerReport.attributionComplete); CHECK(report.ownerReport.incompleteReasons==report.incompleteReasons);
    CHECK((report.incompleteReasons & static_cast<uint32_t>(Reason::HandleExportCapacity))!=0);
    CHECK_FALSE(l.Snapshot().attributionComplete);
}
TEST_CASE("Shared retirement scheduling follows actual dispatch and every diagnostic CPU detach", "[wgpu][geometry-owner-ledger]")
{
    using namespace Poseidon::render;
    std::array<int,3> order{0,1,2};
    do
    {
        GeometryOwnerLedger ledger; REQUIRE(ledger.BeginEpoch(7));
        REQUIRE(ledger.AllocationCreated(7,91,100,20,Kind::Retained,7001));
        REQUIRE(ledger.CpuBorrowerAttached(7,101,91)); REQUIRE(ledger.CpuBorrowerAttached(7,102,91));
        auto epoch=std::make_shared<SharedMeshEpoch>();
        auto lifetime=std::make_shared<SharedRetainedMeshLifetime>(91,epoch);
        SharedRetainedMeshLifetime::Borrower older,newer;
        REQUIRE(older.Attach(lifetime)); REQUIRE(newer.Attach(lifetime));
        REQUIRE(older.Bind(7001)); REQUIRE(newer.Bind(7001));
        size_t dispatched=0;
        for(size_t step=0;step<order.size();++step)
        {
            uint64_t handle=0;
            if(order[step]==0)
            {
                ledger.ProducerOwnershipReleased(7,91);
                handle=lifetime->ReleaseProducer(7001);
                lifetime->Dispatch(handle,[&](uint64_t h) { REQUIRE(h==7001); ++dispatched; });
                if(handle) ledger.AllocationRetired(7,91); // owner-side producer retirement path
            }
            else
            {
                auto& borrower=order[step]==1?older:newer;
                ledger.QueueCpuBorrowerDetached(7,order[step]==1?101:102);
                REQUIRE(borrower.Producer()==91);
                REQUIRE(borrower.Release(handle));
                bool actual=false;
                borrower.Dispatch(handle,[&](uint64_t h) { REQUIRE(h==7001); ++dispatched; actual=true; });
                if(actual) ledger.QueueAllocationRetirementScheduled(7,borrower.Producer());
            }
            GeometryOwnerLedger::HandleRecord record;
            REQUIRE(ledger.SnapshotHandle(7,91,record));
            CHECK(record.scheduledRetired==(step==2));
            CHECK(dispatched==(step==2?1:0));
            CHECK(ledger.Snapshot().attributionComplete);
        }
        const auto report=ledger.Snapshot();
        CHECK(report.liveCpuBorrowers==0); CHECK(report.zeroPersistentOwnerBytes==0);
        CHECK(report.observedProducerEstimatedVertexBytes==0);
        CHECK_FALSE(report.ownershipCoverageComplete); CHECK(report.reclaimableBytes==0);
        CHECK(report.mailboxPeakKnownCapacityBytes>0);
    } while(std::next_permutation(order.begin(),order.end()));
}

TEST_CASE("Typed ownership mailbox is bounded and lost retirement evidence stays incomplete", "[wgpu][geometry-owner-ledger]")
{
    GeometryOwnerLedger ledger({8,8,8,1}); REQUIRE(ledger.BeginEpoch(1));
    REQUIRE(ledger.AllocationCreated(1,4,10,2,Kind::Retained,44));
    REQUIRE(ledger.CpuBorrowerAttached(1,9,4)); ledger.ProducerOwnershipReleased(1,4);
    ledger.QueueCpuBorrowerDetached(1,9);
    ledger.QueueAllocationRetirementScheduled(1,4); // shared cap1, not a separate allowance
    GeometryOwnerLedger::HandleRecord record;
    REQUIRE(ledger.SnapshotHandle(1,4,record)); CHECK(record.cpuBorrowers==0); CHECK_FALSE(record.scheduledRetired);
    const auto report=ledger.Snapshot(); CHECK_FALSE(report.attributionComplete);
    CHECK(report.droppedOwnershipEvents==1); CHECK(report.droppedDetachEvents==report.droppedOwnershipEvents);
    CHECK(report.droppedRetirementEvents==1); CHECK(report.scheduledRetirementEvents==0);
    CHECK(report.queuedOwnershipEvents==0); CHECK(report.queuedDetachEvents==0); CHECK(report.queuedRetirementEvents==0);
    CHECK(report.mailboxKnownCapacityBytes==0); CHECK(report.mailboxPeakKnownCapacityBytes>0);
}

TEST_CASE("Scheduled retirement validates remaining owners without recursively draining", "[wgpu][geometry-owner-ledger]")
{
    GeometryOwnerLedger ledger; REQUIRE(ledger.BeginEpoch(1));
    REQUIRE(ledger.AllocationCreated(1,4,10,2,Kind::Retained,44));
    REQUIRE(ledger.CpuBorrowerAttached(1,9,4));
    // Invalid reversed producer metadata order is not quietly reordered into
    // complete evidence. Real destruction enqueues detach BEFORE scheduling.
    ledger.QueueAllocationRetirementScheduled(1,4); ledger.QueueCpuBorrowerDetached(1,9);
    const auto report=ledger.Snapshot(); CHECK(report.liveCpuBorrowers==0);
    CHECK_FALSE(report.attributionComplete); CHECK(report.scheduledRetirementEvents==1);
    CHECK((report.incompleteReasons&static_cast<uint32_t>(Reason::Protocol))!=0);
}

TEST_CASE("Closed or stale scheduling cannot mutate a replacement epoch", "[wgpu][geometry-owner-ledger]")
{
    using namespace Poseidon::render;
    GeometryOwnerLedger ledger; REQUIRE(ledger.BeginEpoch(1));
    REQUIRE(ledger.AllocationCreated(1,4,10,2,Kind::Retained,44));
    auto epoch=std::make_shared<SharedMeshEpoch>();
    auto lifetime=std::make_shared<SharedRetainedMeshLifetime>(4,epoch);
    SharedRetainedMeshLifetime::Borrower borrower;
    REQUIRE(borrower.Attach(lifetime)); REQUIRE(borrower.Bind(44));
    REQUIRE(lifetime->ReleaseProducer(44)==0); epoch->Close();
    uint64_t handle; REQUIRE(borrower.Release(handle)); CHECK(handle==0);
    bool actual=false; borrower.Dispatch(44,[&](uint64_t) { actual=true; });
    CHECK_FALSE(actual); CHECK(ledger.Snapshot().scheduledRetirementEvents==0);
    ledger.CloseEpoch(1); ledger.QueueAllocationRetirementScheduled(1,4);
    REQUIRE(ledger.BeginEpoch(2)); REQUIRE(ledger.AllocationCreated(2,4,30,4,Kind::Retained,0x20000002c));
    ledger.QueueCpuBorrowerDetached(1,9); ledger.QueueAllocationRetirementScheduled(1,4);
    GeometryOwnerLedger::HandleRecord record;
    REQUIRE_FALSE(ledger.SnapshotHandle(1,4,record));
    REQUIRE(ledger.SnapshotHandle(2,4,record)); CHECK_FALSE(record.scheduledRetired); CHECK(record.producerOwned);
    CHECK(record.rendererHandle==0x20000002c); CHECK(ledger.Snapshot().attributionComplete);
    CHECK(ledger.Snapshot().scheduledRetirementEvents==0);
    CHECK_FALSE(ledger.SnapshotHandle(2,999,record)); CHECK(record.allocationId==0);
}

TEST_CASE("Mesh record closure refuses partial malformed and overflowing pool evidence", "[wgpu][mesh-record-closure]")
{
    using namespace Poseidon::render;
    MeshRecordClosureInput all{true,true,3,3,2,0,0,0,100,20,2,120};
    CHECK(EvaluateMeshRecordClosure(all).recordsComplete);
    CHECK(EvaluateMeshRecordClosure(all).residualValid);
    all.present=1; all.vertexBytes=50; all.indexBytes=10;
    const auto partial=EvaluateMeshRecordClosure(all); CHECK_FALSE(partial.recordsComplete);
    CHECK(partial.residualValid); CHECK(partial.unattributedBytes==60);
    SECTION("missing scope") { all.sourceScopeValid=false; CHECK_FALSE(EvaluateMeshRecordClosure(all).scopeValid); }
    SECTION("truncated") { all.inspected=2; CHECK_FALSE(EvaluateMeshRecordClosure(all).residualValid); }
    SECTION("duplicate generational key") { all.duplicates=1; CHECK_FALSE(EvaluateMeshRecordClosure(all).residualValid); }
    SECTION("failed collector") { all.factsComplete=false; CHECK_FALSE(EvaluateMeshRecordClosure(all).residualValid); }
    SECTION("invalid key") { all.invalid=1; CHECK_FALSE(EvaluateMeshRecordClosure(all).residualValid); }
    SECTION("overflow") { all.vertexBytes=UINT64_MAX; all.indexBytes=1; CHECK_FALSE(EvaluateMeshRecordClosure(all).residualValid); }
    SECTION("foreign totals exceed actual pool") { all.poolLiveBytes=1; CHECK_FALSE(EvaluateMeshRecordClosure(all).residualValid); }
    SECTION("present count exceeds inspected") { all.inspected=all.requested=0; CHECK_FALSE(EvaluateMeshRecordClosure(all).residualValid); }
    SECTION("present count exceeds actual records") { all.liveRecords=0; CHECK_FALSE(EvaluateMeshRecordClosure(all).residualValid); }
}

TEST_CASE("Exact absent retirement metadata prunes history without ownership or resurrection", "[wgpu][geometry-owner-ledger][geometry-history]")
{
    GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(1)); REQUIRE(l.AllocationCreated(1,1,20,4,Kind::Cpu,0x10000000a));
    REQUIRE(l.CpuBorrowerAttached(1,1,1)); l.ProducerOwnershipReleased(1,1); l.NoteCpuCreateSettled(1,1);
    l.QueueCpuBorrowerDetached(1,1); l.QueueAllocationRetirementScheduled(1,1);
    GeometryOwnerLedger::HandleRecord row; REQUIRE(l.TakeRetirementCandidates(1,&row,1)==1);
    CHECK(row.rendererHandle==0x10000000a); CHECK_FALSE(l.ConfirmRetirementObservation(1,1,0x20000000a,true));
    CHECK_FALSE(l.ConfirmRetirementObservation(2,1,row.rendererHandle,true));
    CHECK_FALSE(l.ConfirmRetirementObservation(1,1,row.rendererHandle,false)); // Present / invalid cut retains metadata
    REQUIRE(l.TakeRetirementCandidates(1,&row,1)==1);
    CHECK(l.ConfirmRetirementObservation(1,1,row.rendererHandle,true));
    auto report=l.Snapshot(); CHECK(report.trackedAllocations==0); CHECK(report.trackedBorrowerRecords==0);
    CHECK(report.cumulativePrunedAllocations==1); CHECK(report.cumulativePrunedBorrowers==1);
    CHECK(report.attributionComplete); CHECK_FALSE(report.ownershipCoverageComplete); CHECK(report.reclaimableBytes==0);
    l.QueueCpuBorrowerDetached(1,1); CHECK(l.Snapshot().trackedBorrowerRecords==0);
    CHECK_FALSE(l.AllocationCreated(1,1,20,4,Kind::Cpu,0x20000000a)); CHECK_FALSE(l.Snapshot().attributionComplete);
    REQUIRE(l.BeginEpoch(2)); REQUIRE(l.AllocationCreated(2,1,20,4,Kind::Cpu,0x20000000a));
    CHECK(l.Snapshot().cumulativePrunedAllocations==0); CHECK(l.Snapshot().attributionComplete);
}
TEST_CASE("Early cancellation tombstones persist until monotonic CPU operation settles", "[wgpu][geometry-owner-ledger][geometry-history]")
{
    GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(1));
    l.QueueCpuBorrowerDetached(1,9); l.DrainDetachMailbox(); CHECK(l.Snapshot().trackedBorrowerRecords==1);
    REQUIRE(l.AllocationCreated(1,9,20,4,Kind::Cpu,90)); CHECK_FALSE(l.CpuBorrowerAttached(1,9,9));
    CHECK(l.Snapshot().trackedBorrowerRecords==1); l.NoteCpuCreateSettled(1,9);
    CHECK(l.Snapshot().trackedBorrowerRecords==0); l.QueueCpuBorrowerDetached(1,9); l.DrainDetachMailbox();
    CHECK(l.Snapshot().trackedBorrowerRecords==0); CHECK_FALSE(l.CpuBorrowerAttached(1,9,9));
    CHECK(l.Snapshot().attributionComplete);
    l.NoteCpuCreateSettled(1,8); CHECK_FALSE(l.Snapshot().attributionComplete); // no silent out-of-order contract relaxation
}
TEST_CASE("Retirement candidate queue is bounded and never prunes Present or owned records", "[wgpu][geometry-owner-ledger][geometry-history]")
{
    GeometryOwnerLedger l({4,4,4,4,1}); REQUIRE(l.BeginEpoch(1));
    REQUIRE(l.AllocationCreated(1,1,20,4,Kind::Retained,90)); REQUIRE(l.AllocationCreated(1,2,20,4,Kind::Retained,91));
    l.AllocationRetired(1,1); l.AllocationRetired(1,2); auto report=l.Snapshot();
    CHECK(report.queuedRetirementCandidates==1); CHECK(report.candidateKnownCapacityBytes>0); CHECK_FALSE(report.attributionComplete);
    GeometryOwnerLedger::HandleRecord row; REQUIRE(l.TakeRetirementCandidates(1,&row,1)==1);
    CHECK_FALSE(l.ConfirmRetirementObservation(1,row.allocationId,row.rendererHandle,false));
    CHECK(l.Snapshot().trackedAllocations==2); CHECK(l.Snapshot().queuedRetirementCandidates==1);
    GeometryOwnerLedger owned; REQUIRE(owned.BeginEpoch(1)); REQUIRE(owned.AllocationCreated(1,1,20,4,Kind::Retained,90));
    REQUIRE(owned.ModelLodAttached(1,1,0,1)); owned.AllocationRetired(1,1);
    CHECK(owned.TakeRetirementCandidates(1,&row,1)==0); CHECK_FALSE(owned.ConfirmRetirementObservation(1,1,90,true));
    owned.ModelRetired(1,1); REQUIRE(owned.TakeRetirementCandidates(1,&row,1)==1);
    CHECK(owned.ConfirmRetirementObservation(1,1,90,true)); CHECK_FALSE(owned.Snapshot().attributionComplete); // prior protocol sticky
}
TEST_CASE("Retirement maintenance consumes at most32 candidates in one batch", "[wgpu][geometry-owner-ledger][geometry-history]")
{
    GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(1));
    for(uint64_t id=1;id<=40;++id) { REQUIRE(l.AllocationCreated(1,id,20,4,Kind::Retained,id+90)); l.AllocationRetired(1,id); }
    std::array<GeometryOwnerLedger::HandleRecord,40> rows;
    CHECK(l.TakeRetirementCandidates(1,rows.data(),rows.size())==32); CHECK(l.Snapshot().queuedRetirementCandidates==8);
}

TEST_CASE("Detached failed-publication history prunes with later FIFO progress and no resurrection", "[wgpu][geometry-owner-ledger][geometry-history]")
{
    GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(1));
    l.QueueCpuBorrowerDetached(1,9); l.DrainDetachMailbox(); CHECK(l.Snapshot().trackedBorrowerRecords==1);
    l.NoteCpuCreateSettled(1,10); // ID9 failed before enqueue, subsequent creator is authoritative later progress
    GeometryOwnerLedger::HandleRecord row;
    CHECK(l.TakeRetirementCandidates(1,&row,1)==0); CHECK(l.Snapshot().trackedBorrowerRecords==0);
    CHECK(l.Snapshot().cumulativePrunedBorrowers==1);
    l.QueueCpuBorrowerDetached(1,9); l.DrainDetachMailbox(); CHECK(l.Snapshot().trackedBorrowerRecords==0);
    REQUIRE(l.AllocationCreated(1,11,20,4,Kind::Retained,91)); CHECK_FALSE(l.CpuBorrowerAttached(1,9,11));
    CHECK(l.Snapshot().attributionComplete);
}

TEST_CASE("One selected diagnostic record survives automatic metadata retirement until explicit unpin", "[wgpu][geometry-owner-ledger][geometry-history]")
{
    GeometryOwnerLedger l; REQUIRE(l.BeginEpoch(1)); REQUIRE(l.AllocationCreated(1,1,20,4,Kind::Retained,90));
    REQUIRE(l.SetSelectedDiagnosticPin(1,1,true)); l.AllocationRetired(1,1);
    GeometryOwnerLedger::HandleRecord row; CHECK(l.TakeRetirementCandidates(1,&row,1)==0);
    REQUIRE(l.SnapshotHandle(1,1,row)); CHECK(row.scheduledRetired); CHECK_FALSE(l.ConfirmRetirementObservation(1,1,90,true));
    REQUIRE(l.SetSelectedDiagnosticPin(1,1,false)); REQUIRE(l.TakeRetirementCandidates(1,&row,1)==1);
    CHECK(l.ConfirmRetirementObservation(1,1,90,true)); CHECK_FALSE(l.SnapshotHandle(1,1,row)); CHECK(l.Snapshot().attributionComplete);
}
