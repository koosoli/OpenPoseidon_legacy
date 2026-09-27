#include <catch2/catch_test_macros.hpp>
#include <WgpuRenderer/SharedRetainedMeshLifetime.hpp>
#include <array>
#include <algorithm>
#include <condition_variable>
#include <future>
#include <vector>
using namespace Poseidon::render;

TEST_CASE("Shared retained mesh retirement waits for producer and every CPU borrower", "[shared-retained-mesh]")
{
    // Every release order includes both formerly unsafe newest-first cases.
    std::array<int, 3> order{0, 1, 2};
    do
    {
        auto epoch = std::make_shared<SharedMeshEpoch>();
        auto group = std::make_shared<SharedRetainedMeshLifetime>(41, epoch);
        SharedRetainedMeshLifetime::Borrower older, newer;
        REQUIRE(older.Attach(group)); REQUIRE(newer.Attach(group));
        REQUIRE(group->SnapshotOwners().boundBorrowers == 0);
        const uint64_t rendererHandle = (uint64_t(17) << 32) | 9;
        REQUIRE(older.Bind(rendererHandle)); REQUIRE(newer.Bind(rendererHandle));
        REQUIRE(group->SnapshotOwners().boundBorrowers == 2);
        REQUIRE(older.Bind(rendererHandle)); // repeated bind is not another owner
        REQUIRE(group->SnapshotOwners().boundBorrowers == 2);
        std::vector<uint64_t> retired;
        for (size_t step = 0; step < order.size(); ++step)
        {
            uint64_t handle = 0;
            if (order[step] == 0) handle = group->ReleaseProducer(rendererHandle);
            else REQUIRE((order[step] == 1 ? older : newer).Release(handle));
            group->Dispatch(handle, [&](uint64_t value) { retired.push_back(value); });
            REQUIRE(retired.size() == (step == 2 ? 1 : 0));
        }
        REQUIRE(retired[0] == rendererHandle);
        REQUIRE(group->SnapshotOwners().boundBorrowers == 0);
        REQUIRE_FALSE(group->SnapshotOwners().producerOwned);
        uint64_t duplicate = 42;
        REQUIRE(older.Release(duplicate)); REQUIRE(duplicate == 0);
        REQUIRE(newer.Release(duplicate)); REQUIRE(duplicate == 0);
        REQUIRE(group->ReleaseProducer(rendererHandle) == 0);
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST_CASE("Queued shared borrowers cancel or fall back without lending a mesh identity", "[shared-retained-mesh]")
{
    auto epoch = std::make_shared<SharedMeshEpoch>();
    auto group = std::make_shared<SharedRetainedMeshLifetime>(101, epoch);
    SharedRetainedMeshLifetime::Borrower bound, pending, cancelled;
    REQUIRE(bound.Attach(group)); REQUIRE(bound.Bind(0x100000003));
    REQUIRE(pending.Attach(group)); REQUIRE(cancelled.Attach(group));
    uint64_t handle = 999;
    REQUIRE(cancelled.Release(handle)); REQUIRE(handle == 0); // create skipped before drain
    REQUIRE(group->ReleaseProducer(0x100000003) == 0);
    REQUIRE(pending.FallBackToStandalone() == 0); // original stored vertices create their own mesh
    REQUIRE_FALSE(pending.Release(handle)); REQUIRE(handle == 0);
    REQUIRE(bound.Release(handle)); REQUIRE(handle == 0x100000003);
    // Same Rust slot, different generation and producer: no old-ticket alias.
    auto replacement = std::make_shared<SharedRetainedMeshLifetime>(102, epoch);
    SharedRetainedMeshLifetime::Borrower next;
    REQUIRE(next.Attach(replacement)); REQUIRE(next.Bind(0x200000003));
    REQUIRE_FALSE(next.Bind(0x100000003));
    REQUIRE_FALSE(pending.Bind(0x200000003));
    REQUIRE(replacement->ReleaseProducer(0x200000003) == 0);
    REQUIRE(next.Release(handle)); REQUIRE(handle == 0x200000003);
}

TEST_CASE("Last queued fallback retires the old allocation and standalone path remains independent", "[shared-retained-mesh]")
{
    auto epoch = std::make_shared<SharedMeshEpoch>();
    auto group = std::make_shared<SharedRetainedMeshLifetime>(8, epoch);
    SharedRetainedMeshLifetime::Borrower queued;
    REQUIRE(queued.Attach(group));
    REQUIRE(group->ReleaseProducer(17) == 0);
    REQUIRE(queued.FallBackToStandalone() == 17);
    REQUIRE(queued.FallBackToStandalone() == 0);
    uint64_t ignored;
    REQUIRE_FALSE(queued.Release(ignored));
    REQUIRE_FALSE(queued.Attach(group)); // no rebinding an existing immutable association
}

TEST_CASE("Closed engine epoch suppresses late borrowed uploader access", "[shared-retained-mesh]")
{
    auto epoch = std::make_shared<SharedMeshEpoch>();
    auto group = std::make_shared<SharedRetainedMeshLifetime>(1, epoch);
    SharedRetainedMeshLifetime::Borrower older, newer;
    REQUIRE(older.Attach(group)); REQUIRE(newer.Attach(group));
    REQUIRE(older.Bind(33)); REQUIRE(newer.Bind(33));
    REQUIRE(group->ReleaseProducer(33) == 0);
    epoch->Close();
    uint64_t handle;
    REQUIRE(older.Release(handle)); REQUIRE(handle == 0);
    REQUIRE(newer.Release(handle)); REQUIRE(handle == 0);
    bool called = false;
    group->Dispatch(33, [&](uint64_t) { called = true; });
    REQUIRE_FALSE(called);
    SharedRetainedMeshLifetime::Borrower late;
    REQUIRE_FALSE(late.Attach(group));
    auto newEpoch = std::make_shared<SharedMeshEpoch>();
    auto replacement = std::make_shared<SharedRetainedMeshLifetime>(1, newEpoch);
    REQUIRE(late.Attach(replacement)); REQUIRE(late.Bind(33));
    REQUIRE(replacement->ReleaseProducer(33) == 0);
    REQUIRE(late.Release(handle)); REQUIRE(handle == 33);
}

TEST_CASE("Epoch close waits for an already-dispatched enqueue but prevents later enqueues", "[shared-retained-mesh]")
{
    auto epoch = std::make_shared<SharedMeshEpoch>();
    std::promise<void> entered, release, closeStarted;
    auto released = release.get_future().share();
    auto dispatch = std::async(std::launch::async, [&] {
        epoch->Dispatch(22, [&](uint64_t) { entered.set_value(); released.wait(); });
    });
    entered.get_future().wait();
    // RAII release prevents a failed assertion from hanging future destruction.
    std::future<void> close;
    struct Release { std::promise<void>& p; ~Release() { try { p.set_value(); } catch (...) {} } } unblock{release};
    close = std::async(std::launch::async, [&] { closeStarted.set_value(); epoch->Close(); });
    closeStarted.get_future().wait();
    REQUIRE(close.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    release.set_value();
    dispatch.get(); close.get();
    bool called = false;
    epoch->Dispatch(22, [&](uint64_t) { called = true; });
    REQUIRE_FALSE(called);
}

TEST_CASE("Unpublished producer cancellation cannot lend a new borrower a GPU identity", "[shared-retained-mesh]")
{
    auto epoch = std::make_shared<SharedMeshEpoch>();
    auto group = std::make_shared<SharedRetainedMeshLifetime>(12, epoch);
    SharedRetainedMeshLifetime::Borrower pending, late;
    REQUIRE(pending.Attach(group));
    REQUIRE(group->ReleaseProducer(0) == 0); // MeshCreate enqueue failed: no renderer handle exists.
    REQUIRE_FALSE(group->SnapshotOwners().producerOwned);
    REQUIRE_FALSE(late.Attach(group));
    REQUIRE(pending.FallBackToStandalone() == 0);
    uint64_t ignored;
    REQUIRE_FALSE(pending.Release(ignored));
    REQUIRE_FALSE(pending.Bind(42));
}
