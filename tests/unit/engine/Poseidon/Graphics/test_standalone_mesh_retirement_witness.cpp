#include <catch2/catch_test_macros.hpp>
#include <WgpuRenderer/StandaloneMeshRetirementWitness.hpp>
#include <WgpuRenderer/GeometryOwnerLedger.hpp>
#include <vector>
#include <algorithm>
using namespace Poseidon::render;
TEST_CASE("Standalone witness retires only actual full handles exactly once", "[wgpu][standalone-mesh-witness]")
{
    auto epoch=std::make_shared<SharedMeshEpoch>();
    auto budget=std::make_shared<StandaloneMeshWitnessBudget>();
    auto witness=budget->Create(11,epoch); REQUIRE(witness);
    CHECK(witness->Producer()==11); CHECK(witness->Bind(0x20000000a)==0);
    CHECK(witness->Snapshot().handle==0x20000000a); CHECK_FALSE(witness->Snapshot().released);
    size_t callbacks=0;
    auto dispatch=[&](uint64_t handle) { CHECK(handle==0x20000000a); ++callbacks; };
    witness->Dispatch(witness->Release(),dispatch);
    witness->Dispatch(witness->Release(),dispatch); CHECK(callbacks==1);
    CHECK(witness->Snapshot().released);
    CHECK(budget->Snapshot().live==1); CHECK(budget->Snapshot().bytes==StandaloneMeshWitnessBudget::Charge);
    witness.reset(); CHECK(budget->Snapshot().live==0); CHECK(budget->Snapshot().bytes==0);
}
TEST_CASE("Standalone cancellation before creation never invents a destroy handle", "[wgpu][standalone-mesh-witness]")
{
    auto epoch=std::make_shared<SharedMeshEpoch>();
    StandaloneMeshRetirementWitness cancelled(9,epoch);
    CHECK(cancelled.Release()==0); CHECK(cancelled.Release()==0);
    CHECK(cancelled.Snapshot().handle==0); CHECK(cancelled.Bind(0)==0);
    CHECK(cancelled.Snapshot().handle==0);
    size_t callbacks=0; cancelled.Dispatch(cancelled.Release(),[&](uint64_t) { ++callbacks; }); CHECK(callbacks==0);
}
TEST_CASE("Standalone released before actual create is recorded then scheduled exactly once", "[wgpu][standalone-mesh-witness]")
{
    auto epoch=std::make_shared<SharedMeshEpoch>();
    StandaloneMeshRetirementWitness witness(9,epoch);
    GeometryOwnerLedger ledger; REQUIRE(ledger.BeginEpoch(1));
    ledger.QueueCpuBorrowerDetached(1,9); CHECK(witness.Release()==0);
    // Exact owner operation ordering used by CpuMeshCreate: allocation/borrower
    // evidence first, then late Bind claims the actual freshly-created handle.
    REQUIRE(ledger.AllocationCreated(1,9,128,24,GeometryOwnerLedger::ProducerKind::Cpu,0x100000005));
    CHECK_FALSE(ledger.CpuBorrowerAttached(1,9,9)); ledger.ProducerOwnershipReleased(1,9);
    size_t callbacks=0;
    witness.Dispatch(witness.Bind(0x100000005),[&](uint64_t h) { CHECK(h==0x100000005); ++callbacks; });
    if(callbacks) ledger.QueueAllocationRetirementScheduled(1,9);
    const auto report=ledger.Snapshot(); CHECK(callbacks==1); CHECK(report.scheduledRetirementEvents==1);
    CHECK(report.liveCpuBorrowers==0); CHECK(report.attributionComplete); CHECK_FALSE(report.ownershipCoverageComplete);
    CHECK(witness.Release()==0);
}
TEST_CASE("Retained fallback preserves standalone witness independent full handle", "[wgpu][standalone-mesh-witness]")
{
    auto epoch=std::make_shared<SharedMeshEpoch>();
    auto retained=std::make_shared<SharedRetainedMeshLifetime>(3,epoch);
    SharedRetainedMeshLifetime::Borrower borrower; REQUIRE(borrower.Attach(retained));
    CHECK(retained->ReleaseProducer(0)==0); CHECK(borrower.FallBackToStandalone()==0);
    StandaloneMeshRetirementWitness standalone(4,epoch); CHECK(standalone.Bind(0x300000008)==0);
    uint64_t ignored=0; CHECK_FALSE(borrower.Release(ignored));
    size_t callbacks=0; standalone.Dispatch(standalone.Release(),[&](uint64_t h) { CHECK(h==0x300000008); ++callbacks; });
    CHECK(callbacks==1); CHECK(standalone.Release()==0);
}
TEST_CASE("Standalone epoch closure rejects late backend dispatch and numeric facts", "[wgpu][standalone-mesh-witness]")
{
    auto epoch=std::make_shared<SharedMeshEpoch>(); StandaloneMeshRetirementWitness witness(9,epoch);
    CHECK(witness.Bind(0x100000009)==0); epoch->Close();
    size_t callbacks=0; witness.Dispatch(witness.Release(),[&](uint64_t) { ++callbacks; }); CHECK(callbacks==0);
    CHECK(witness.Release()==0); CHECK(witness.Snapshot().released);
}
TEST_CASE("Standalone witness metadata remains capped until all aliases release", "[wgpu][standalone-mesh-witness]")
{
    auto epoch=std::make_shared<SharedMeshEpoch>(); auto budget=std::make_shared<StandaloneMeshWitnessBudget>();
    std::vector<std::shared_ptr<StandaloneMeshRetirementWitness>> witnesses;
    const size_t bound=std::min(StandaloneMeshWitnessBudget::MaxWitnesses,StandaloneMeshWitnessBudget::MaxBytes/StandaloneMeshWitnessBudget::Charge);
    for(size_t i=0;i<bound;++i) { auto w=budget->Create(i+1,epoch); REQUIRE(w); witnesses.push_back(std::move(w)); }
    CHECK_FALSE(budget->Create(bound+1,epoch)); CHECK(budget->Snapshot().refused==1);
    CHECK(budget->Snapshot().bytes<=StandaloneMeshWitnessBudget::MaxBytes);
    auto alias=witnesses.back(); witnesses.pop_back(); CHECK_FALSE(budget->Create(bound+1,epoch));
    alias.reset(); auto replacement=budget->Create(bound+1,epoch); REQUIRE(replacement);
    witnesses.clear(); replacement.reset(); CHECK(budget->Snapshot().bytes==0); CHECK(budget->Snapshot().live==0);
}

TEST_CASE("Attributed standalone suppression preserves legacy fallback until responsibility taken", "[wgpu][standalone-mesh-witness]")
{
    SharedRetainedMeshLifetime::Borrower plain;
    uint64_t handle=99; CHECK_FALSE(plain.Release(handle)); CHECK(handle==0);
    // Refused optional witness leaves the original false/standalone branch.
    CHECK_FALSE(plain.Release(handle));
    plain.SuppressHandledStandaloneRetirement();
    CHECK(plain.Release(handle)); CHECK(handle==0); CHECK(plain.Release(handle));
    CHECK(plain.Producer()==0); // no invented synthetic producer association
}
TEST_CASE("Standalone suppression survives pending late bind and closed epoch", "[wgpu][standalone-mesh-witness]")
{
    auto epoch=std::make_shared<SharedMeshEpoch>();
    StandaloneMeshRetirementWitness witness(12,epoch);
    SharedRetainedMeshLifetime::Borrower legacy;
    uint64_t ignored=0; CHECK_FALSE(legacy.Release(ignored));
    legacy.SuppressHandledStandaloneRetirement(); CHECK(witness.Release()==0);
    size_t dispatched=0;
    witness.Dispatch(witness.Bind(0x40000000a),[&](uint64_t h) { CHECK(h==0x40000000a); ++dispatched; });
    CHECK(dispatched==1); CHECK(legacy.Release(ignored)); CHECK(ignored==0);
    epoch->Close(); CHECK(legacy.Release(ignored));
    witness.Dispatch(witness.Release(),[&](uint64_t) { ++dispatched; }); CHECK(dispatched==1);
}
TEST_CASE("Shared fallback suppression keeps immutable retained association detached", "[wgpu][standalone-mesh-witness]")
{
    auto epoch=std::make_shared<SharedMeshEpoch>();
    auto retained=std::make_shared<SharedRetainedMeshLifetime>(3,epoch);
    SharedRetainedMeshLifetime::Borrower borrower; REQUIRE(borrower.Attach(retained));
    CHECK(retained->ReleaseProducer(0)==0); CHECK(borrower.FallBackToStandalone()==0);
    uint64_t ignored=0; CHECK_FALSE(borrower.Release(ignored));
    borrower.SuppressHandledStandaloneRetirement(); CHECK(borrower.Release(ignored)); CHECK(ignored==0);
    CHECK(borrower.Producer()==3); CHECK(retained->SnapshotOwners().boundBorrowers==0);
}
