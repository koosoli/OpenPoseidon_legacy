#include <catch2/catch_test_macros.hpp>
#include <WgpuRenderer/ParkedGeometryReport.hpp>
#include <WgpuRenderer/GeometryOwnerLedger.hpp>
#include <algorithm>
#include <limits>
using namespace Poseidon::render;
namespace {
ParkedGeometryObservation Present(uint64_t id,uint64_t bytes,uint64_t cpu,uint64_t links,uint64_t selected)
{
    ParkedGeometryObservation v;
    v.allocation=id;v.epoch=7;v.handle=id+100;v.cpu=cpu;v.modelLinks=links;v.selectedLinks=selected;
    v.vertexBytes=bytes;v.indexBytes=bytes/5;v.producer=true;v.factValid=true;v.state=1;return v;
}
}
TEST_CASE("Parked candidates partition shared and outside owners without reclaim claims", "[wgpu][parked-geometry-report]")
{
    ParkedGeometryCapture capture;capture.models={1,2};capture.allocations={10,11,12,13,14,10};capture.metadataVisits=8;
    auto absent=Present(13,0,0,0,0);absent.producer=false;absent.retired=true;absent.state=2;
    auto retired=Present(14,400,0,1,1);retired.retired=true;
    std::vector<ParkedGeometryObservation> observations={Present(10,100,2,1,1),Present(11,200,0,2,2),
        Present(12,300,3,3,2),absent,retired};
    const auto result=ClassifyParkedGeometry(capture,7,true,observations);
    CHECK_FALSE(result.complete);CHECK(result.uniqueAllocations==5);CHECK(result.knownCandidateBytes==1200);
    CHECK(result.cpuBorrowed==1);CHECK(result.cpuBorrowedBytes==120);
    CHECK(result.unborrowed==1);CHECK(result.unborrowedBytes==240);
    CHECK(result.outsideLinked==1);CHECK(result.outsideLinkedBytes==360);
    CHECK(result.absentRetired==1);CHECK(result.unknown==1);CHECK(result.unknownKnownBytes==480);
    CHECK(result.cpuBorrowed+result.unborrowed+result.outsideLinked+result.absentRetired+result.unknown==result.uniqueAllocations);
    CHECK(result.cpuBorrowedBytes+result.unborrowedBytes+result.outsideLinkedBytes+result.unknownKnownBytes==result.knownCandidateBytes);
    capture.allocations.pop_back();capture.allocations.pop_back(); // Remove duplicate and uncertain retired-present ID.
    const auto complete=ClassifyParkedGeometry(capture,7,true,observations);
    CHECK(complete.complete);CHECK(complete.unknown==0);CHECK(complete.knownCandidateBytes==720);
}
TEST_CASE("Parked join refuses uncertain cuts and malformed numeric evidence", "[wgpu][parked-geometry-report]")
{
    ParkedGeometryCapture capture;capture.models={1};capture.allocations={10};capture.metadataVisits=2;
    const auto original=Present(10,100,0,1,1);
    REQUIRE(ClassifyParkedGeometry(capture,7,true,{original}).complete);
    SECTION("incomplete capture keeps bytes unknown") {
        capture.truncated=true;const auto r=ClassifyParkedGeometry(capture,7,true,{original});
        CHECK_FALSE(r.complete);CHECK(r.unknown==1);CHECK(r.unknownKnownBytes==120);CHECK(r.unborrowed==0);
    }
    SECTION("unsettled joined cut") {CHECK(ClassifyParkedGeometry(capture,7,false,{original}).unknown==1);}
    SECTION("wrong epoch") {auto v=original;v.epoch=8;CHECK(ClassifyParkedGeometry(capture,7,true,{v}).unknown==1);}
    SECTION("duplicate observations") {CHECK(ClassifyParkedGeometry(capture,7,true,{original,original}).unknown==1);}
    SECTION("missing record") {CHECK(ClassifyParkedGeometry(capture,7,true,{}).unknown==1);}
    SECTION("selected links exceed global union") {auto v=original;v.selectedLinks=2;CHECK(ClassifyParkedGeometry(capture,7,true,{v}).unknown==1);}
    SECTION("no selected association cannot certify a stale request") {auto v=original;v.selectedLinks=0;CHECK(ClassifyParkedGeometry(capture,7,true,{v}).unknown==1);}
    SECTION("overflow discards reassuring byte totals") {
        auto v=original;v.vertexBytes=UINT64_MAX;v.indexBytes=1;const auto r=ClassifyParkedGeometry(capture,7,true,{v});
        CHECK_FALSE(r.complete);CHECK(r.unknown==1);CHECK(r.knownCandidateBytes==0);CHECK(r.unborrowed==0);
    }
    SECTION("hard capture limit") {capture.models.resize(257);CHECK_FALSE(ClassifyParkedGeometry(capture,7,true,{original}).complete);}
}
TEST_CASE("Selected ledger tuples and CPU detach share one drained owner cut", "[wgpu][parked-geometry-report]")
{
    GeometryOwnerLedger ledger;using Kind=GeometryOwnerLedger::ProducerKind;
    REQUIRE(ledger.BeginEpoch(7));REQUIRE(ledger.AllocationCreated(7,10,100,20,Kind::Retained,110));
    REQUIRE(ledger.AllocationCreated(7,11,200,40,Kind::Retained,111));
    REQUIRE(ledger.ModelLodAttached(7,5,0,10));REQUIRE(ledger.ModelLodAttached(7,5,0,11));
    REQUIRE(ledger.ModelLodAttached(7,6,1,10));REQUIRE(ledger.CpuBorrowerAttached(7,3,10));
    ledger.QueueCpuBorrowerDetached(7,3);
    const std::vector<uint32_t> models{5};const auto report=ledger.SnapshotHandles(&models);
    REQUIRE(report.selectedLinksComplete);CHECK(report.selectedLinkVisits==4);REQUIRE(report.selectedModelLinks.size()==2);
    CHECK(report.ownerReport.liveCpuBorrowers==0);CHECK(report.queuedDetachEvents==0);
    ParkedGeometryCapture capture;capture.models=models;capture.allocations={10,11};capture.metadataVisits=3;
    std::vector<ParkedGeometryObservation> observed;
    for(const auto& record:report.records) {
        auto v=Present(record.allocationId,record.allocationId==10?100:200,record.cpuBorrowers,record.modelLodLinks,0);
        v.epoch=record.epoch;v.handle=record.rendererHandle;
        for(const auto& tuple:report.selectedModelLinks)if(tuple.allocation==record.allocationId)++v.selectedLinks;
        observed.push_back(v);
    }
    const auto classified=ClassifyParkedGeometry(capture,report.epoch,report.attributionComplete,observed);
    REQUIRE(classified.complete);CHECK(classified.cpuBorrowed==0);CHECK(classified.outsideLinked==1);CHECK(classified.unborrowed==1);
    const std::vector<uint32_t> duplicate{5,5};CHECK_FALSE(ledger.SnapshotHandles(&duplicate).selectedLinksComplete);
    const std::vector<uint32_t> missing{99};CHECK_FALSE(ledger.SnapshotHandles(&missing).selectedLinksComplete);
    const auto ordinary=ledger.SnapshotHandles();CHECK_FALSE(ordinary.selectedLinksRequested);
    CHECK(ordinary.selectedModelLinks.empty());CHECK(ordinary.selectedModelLinks.capacity()==0);
    CHECK(ordinary.attributionComplete); // Diagnostic uncertainty does not poison lifetime bookkeeping.
}
TEST_CASE("Selected model metadata visits stop at a bounded prefix without sticky ledger failure", "[wgpu][parked-geometry-report]")
{
    GeometryOwnerLedger ledger;REQUIRE(ledger.BeginEpoch(1));
    REQUIRE(ledger.AllocationCreated(1,1,20,4,GeometryOwnerLedger::ProducerKind::Retained,90));
    for(uint32_t lod=0;lod<1100;++lod)REQUIRE(ledger.ModelLodAttached(1,5,lod,1));
    const std::vector<uint32_t> models{5};const auto cut=ledger.SnapshotHandles(&models);
    CHECK_FALSE(cut.selectedLinksComplete);CHECK(cut.selectedLinksTruncated);CHECK(cut.selectedLinkVisits==2048);
    CHECK(cut.selectedModelLinks.size()<=2048);CHECK(cut.attributionComplete);
    CHECK(ledger.Snapshot().attributionComplete);
    const std::vector<uint32_t> tooMany(257,5);const auto refused=ledger.SnapshotHandles(&tooMany);
    CHECK_FALSE(refused.selectedLinksComplete);CHECK(refused.selectedLinksTruncated);CHECK(refused.selectedLinkVisits==0);
}
