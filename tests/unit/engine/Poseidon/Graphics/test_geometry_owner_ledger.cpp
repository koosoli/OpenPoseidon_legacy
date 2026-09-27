#include <catch2/catch_test_macros.hpp>
#include <WgpuRenderer/GeometryOwnerLedger.hpp>
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