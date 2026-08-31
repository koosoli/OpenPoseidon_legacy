// Streaming observability: the shared counters actually reach the snapshot both consumers
// read, and the cold-model preparer's request->residency latency is really measured.
//
// The second test drives a REAL ObjectStreamPreparer over a fixture model rather than
// poking counters directly, because the latency fields are written inside the worker /
// Take() paths -- a test that bypassed them would pass with the stamps never taken.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Dev/Diag/StreamingDiag.hpp>
#include <Poseidon/World/Terrain/ObjectStreamPrepare.hpp>

#include "../../test_fixtures.hpp"

#include <chrono>
#include <string>
#include <thread>

TEST_CASE("StreamingDiag: texture counters round-trip into the snapshot", "[streaming][diag]")
{
    Poseidon::Dev::TextureStreamCounters& c = Poseidon::Dev::GTextureStreamCounters();
    const uint64_t uploadsBefore = c.uploads.load(std::memory_order_relaxed);
    const uint64_t readBefore = c.readUs.load(std::memory_order_relaxed);

    c.uploads.fetch_add(3, std::memory_order_relaxed);
    c.readUs.fetch_add(1500, std::memory_order_relaxed);

    const Poseidon::Dev::StreamingSnapshot s = Poseidon::Dev::CollectStreamingSnapshot();
    CHECK(s.tex.uploads == uploadsBefore + 3);
    CHECK(s.tex.readUs == readBefore + 1500);
    // No world in a unit test: the residency section must say so rather than emit zeros.
    CHECK_FALSE(s.residency.valid);
}

TEST_CASE("StreamingDiag: frame roll moves accumulators to last/peak", "[streaming][diag]")
{
    Poseidon::Dev::TextureStreamCounters& c = Poseidon::Dev::GTextureStreamCounters();
    Poseidon::Dev::StreamingFrameRoll(); // flush whatever earlier tests accumulated

    c.frameUploads.fetch_add(2, std::memory_order_relaxed);
    c.frameUploadBytes.fetch_add(4096, std::memory_order_relaxed);
    c.frameUploadUs.fetch_add(700, std::memory_order_relaxed);
    Poseidon::Dev::StreamingFrameRoll();

    Poseidon::Dev::StreamingSnapshot s = Poseidon::Dev::CollectStreamingSnapshot();
    CHECK(s.tex.lastFrameUploads == 2);
    CHECK(s.tex.lastFrameUploadBytes == 4096);
    CHECK(s.tex.lastFrameUploadUs == 700);
    CHECK(s.tex.peakFrameUploadBytes >= 4096);

    // A quiet frame resets "last" but the peak survives it.
    Poseidon::Dev::StreamingFrameRoll();
    s = Poseidon::Dev::CollectStreamingSnapshot();
    CHECK(s.tex.lastFrameUploads == 0);
    CHECK(s.tex.peakFrameUploadBytes >= 4096);
}

TEST_CASE("ObjectStreamPreparer: request->ready and request->taken latency are recorded",
          "[streaming][diag][preparer]")
{
    if (!Poseidon::ObjectStreamPreparer::AsyncEnabled())
    {
        SKIP("WGR_OBJECT_STREAM_ASYNC=0 in this environment");
    }

    Poseidon::ObjectStreamPreparer preparer;
    const std::string path = TestFixtures::GetTestFixturePath("p3d/complex_vehicle.p3d");
    preparer.Reset(&path, 1);
    REQUIRE(preparer.Running());
    REQUIRE(preparer.Request(0));

    // The worker parses a small fixture in well under a second; poll rather than sleep a
    // fixed time so the test is fast on a warm machine and tolerant on a loaded one.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (preparer.Query(0) != Poseidon::ObjectStreamPreparer::State::Ready)
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        REQUIRE(preparer.Query(0) != Poseidon::ObjectStreamPreparer::State::Failed);
        REQUIRE(preparer.Query(0) != Poseidon::ObjectStreamPreparer::State::NotLoose);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    auto model = preparer.Take(0);
    REQUIRE(model != nullptr);

    const Poseidon::ObjectStreamPreparer::Stats stats = preparer.SnapshotStats();
    CHECK(stats.requested == 1);
    CHECK(stats.prepared == 1);
    CHECK(stats.taken == 1);
    CHECK(stats.readyLatencyUsTotal > 0);
    CHECK(stats.readyLatencyUsMax >= stats.readyLatencyUsTotal / stats.prepared);
    // Take happens after Ready, so its latency can never be the smaller one.
    CHECK(stats.takeLatencyUsTotal >= stats.readyLatencyUsTotal);
    CHECK(stats.takeLatencyUsMax >= stats.readyLatencyUsMax);
}

// Request deduplication is structural -- Request() refuses any index that is not Unknown --
// but it was invisible until `coalesced`, and an invisible coalescer cannot be told from one
// that has stopped working. This drives a real preparer, because the counter is bumped inside
// the same lock that reads the state and a test that set it directly would prove nothing.
TEST_CASE("ObjectStreamPreparer: repeated requests coalesce onto the first, and are counted",
          "[streaming][diag][preparer]")
{
    if (!Poseidon::ObjectStreamPreparer::AsyncEnabled())
    {
        SKIP("WGR_OBJECT_STREAM_ASYNC=0 in this environment");
    }

    Poseidon::ObjectStreamPreparer preparer;
    const std::string path = TestFixtures::GetTestFixturePath("p3d/complex_vehicle.p3d");
    preparer.Reset(&path, 1);
    REQUIRE(preparer.Running());

    REQUIRE(preparer.Request(0));
    // Whatever the worker has done with it by now -- Queued, Parsing or Ready -- the slot is
    // tracked, so every further ask must be refused rather than enqueued a second time.
    for (int i = 0; i < 5; ++i)
        CHECK_FALSE(preparer.Request(0));

    Poseidon::ObjectStreamPreparer::Stats stats = preparer.SnapshotStats();
    CHECK(stats.requested == 1); // one enqueue, not six
    CHECK(stats.coalesced + stats.coalescedStuck == 5);
    // The fixture is a loose, parseable file, so none of the refusals may be the sticky kind
    // (NotLoose / Failed) -- those save no parse and are deliberately not counted as dedup.
    CHECK(stats.coalescedStuck == 0);
    CHECK(stats.rejectedFull == 0);

    // Out-of-range asks are neither enqueued nor dedup: they must not move either counter.
    const uint64_t coalescedBefore = stats.coalesced;
    CHECK_FALSE(preparer.Request(99));
    stats = preparer.SnapshotStats();
    CHECK(stats.coalesced == coalescedBefore);
    CHECK(stats.requested == 1);

    // Taking the IR returns the slot to Unknown, so the NEXT ask is a real request again --
    // dedup that never expired would be a leak, not a saving.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (preparer.Query(0) != Poseidon::ObjectStreamPreparer::State::Ready)
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        REQUIRE(preparer.Query(0) != Poseidon::ObjectStreamPreparer::State::Failed);
        REQUIRE(preparer.Query(0) != Poseidon::ObjectStreamPreparer::State::NotLoose);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(preparer.Take(0) != nullptr);
    CHECK(preparer.Request(0));
    CHECK(preparer.SnapshotStats().requested == 2);
}

// The ready bound (WGR_OBJECT_STREAM_ASYNC_READY) is the one preparer bound that reported
// nothing when it bit: a parked worker and an idle worker looked identical from outside.
// The counter must stay at zero for a run that never approaches the limit -- a park counter
// that ticks on ordinary empty-queue waits would be worse than none, because it would read
// as back-pressure on every quiet frame.
TEST_CASE("ObjectStreamPreparer: an idle worker is not counted as a ready-bound park",
          "[streaming][diag][preparer]")
{
    if (!Poseidon::ObjectStreamPreparer::AsyncEnabled())
    {
        SKIP("WGR_OBJECT_STREAM_ASYNC=0 in this environment");
    }

    Poseidon::ObjectStreamPreparer preparer;
    const std::string path = TestFixtures::GetTestFixturePath("p3d/complex_vehicle.p3d");
    preparer.Reset(&path, 1);
    REQUIRE(preparer.Running());
    // Workers start, find nothing, and wait; one model can never fill a ready set of 128.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(preparer.Request(0));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (preparer.Query(0) == Poseidon::ObjectStreamPreparer::State::Queued ||
           preparer.Query(0) == Poseidon::ObjectStreamPreparer::State::Parsing)
    {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // workers back to waiting
    CHECK(preparer.SnapshotStats().readyFullParks == 0);
    CHECK(Poseidon::ObjectStreamPreparer::ReadyLimit() > 1);
}
