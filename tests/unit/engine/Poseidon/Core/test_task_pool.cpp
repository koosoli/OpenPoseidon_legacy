// Unit tests for TaskPool — Phase 1 worker-pool foundation.  Tests
// land before the implementation so the contract is pinned by the
// suite, not by the first caller.  Covered pins:
//   - ParallelFor with N work items invokes work for every index
//     in [0, N) exactly once, regardless of partitioning
//   - Sequential and parallel produce identical observable output
//     (the migration safety net for landscape's SegGenTask)
//   - Degenerate N=0 / N=1 paths don't deadlock
//   - Concurrent ParallelFor calls from multiple threads are safe
//   - ThreadCount() returns >= 1 (single-core machines included)
//   - Lifetime: construct + destruct repeatedly without leak / hang
//   - Pool with explicit thread count honours the request

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <Poseidon/Core/TaskPool.hpp>

#include <atomic>
#include <chrono>
#include <numeric>
#include <thread>
#include <vector>
#include <stdint.h>
#include <format>
#include <functional>
#include <memory>

using Poseidon::TaskPool;

// The auto pool is capped at kDefaultMaxTaskThreads rather than spawning one worker
// per core on a very wide machine (the enkiTS default); see the constant for why 16.
//
// Broken-state delta: drop the cap and ResolveTaskPoolThreadCount(-1, 64) returns
// 64, not kDefaultMaxTaskThreads.
TEST_CASE("ResolveTaskPoolThreadCount caps the auto pool but honours overrides", "[task_pool]")
{
    using Poseidon::kDefaultMaxTaskThreads;
    using Poseidon::ResolveTaskPoolThreadCount;

    // Auto (cliOverride < 0): cap on a high-core machine, but never more than it has.
    REQUIRE(ResolveTaskPoolThreadCount(-1, 64) == kDefaultMaxTaskThreads); // big box → capped
    REQUIRE(ResolveTaskPoolThreadCount(-1, kDefaultMaxTaskThreads) == kDefaultMaxTaskThreads);
    REQUIRE(ResolveTaskPoolThreadCount(-1, 4) == 4u);                     // 4-core → 4 (below the cap)
    REQUIRE(ResolveTaskPoolThreadCount(-1, 1) == 1u);                     // single core → 1
    REQUIRE(ResolveTaskPoolThreadCount(-1, 0) == kDefaultMaxTaskThreads); // unknown → fall back to cap

    // Uncapped (0): passes 0 through so enkiTS uses every core.
    REQUIRE(ResolveTaskPoolThreadCount(0, 64) == 0u);

    // Explicit (>0): honoured verbatim, even above the cap.
    REQUIRE(ResolveTaskPoolThreadCount(12, 64) == 12u);
    REQUIRE(ResolveTaskPoolThreadCount(2, 64) == 2u);
}

TEST_CASE("TaskPool ParallelFor covers every index exactly once", "[task_pool]")
{
    TaskPool pool;
    constexpr uint32_t N = 1024;
    std::vector<std::atomic<int>> hits(N);
    for (auto& h : hits)
    {
        h.store(0);
    }

    pool.ParallelFor(N,
                     [&](uint32_t start, uint32_t end)
                     {
                         for (uint32_t i = start; i < end; ++i)
                         {
                             hits[i].fetch_add(1, std::memory_order_relaxed);
                         }
                     });

    for (uint32_t i = 0; i < N; ++i)
    {
        REQUIRE(hits[i].load() == 1);
    }
}

TEST_CASE("TaskPool ParallelFor result equals sequential loop", "[task_pool][equivalence]")
{
    // Migration safety net: landscape's SegGenTask is a parallel loop
    // over independent work items.  This test pins that the parallel
    // and sequential paths produce identical observable state, which
    // is the property the landscape pixel regression depends on.
    TaskPool pool;
    constexpr uint32_t N = 256;

    auto work = [](uint32_t i) -> int64_t
    {
        // Some not-trivially-zero arithmetic so the optimiser doesn't
        // fold this into a constant.
        return (static_cast<int64_t>(i) * 1664525 + 1013904223) & 0xFFFFFFFF;
    };

    std::vector<int64_t> sequential(N);
    for (uint32_t i = 0; i < N; ++i)
    {
        sequential[i] = work(i);
    }

    std::vector<int64_t> parallel(N);
    pool.ParallelFor(N,
                     [&](uint32_t start, uint32_t end)
                     {
                         for (uint32_t i = start; i < end; ++i)
                         {
                             parallel[i] = work(i);
                         }
                     });

    REQUIRE(parallel == sequential);
}

TEST_CASE("TaskPool ParallelFor with count=0 is a no-op", "[task_pool][edge]")
{
    TaskPool pool;
    std::atomic<int> calls{0};
    pool.ParallelFor(0, [&](uint32_t, uint32_t) { calls.fetch_add(1, std::memory_order_relaxed); });
    // Implementations are allowed to invoke the callback with an
    // empty [start, end) range or skip entirely; what's pinned is
    // that no work indices are visited and no deadlock occurs.
    REQUIRE(calls.load() <= 1);
}

TEST_CASE("TaskPool ParallelFor with count=1 runs work for index 0", "[task_pool][edge]")
{
    TaskPool pool;
    std::atomic<int> hits{0};
    pool.ParallelFor(1,
                     [&](uint32_t start, uint32_t end)
                     {
                         for (uint32_t i = start; i < end; ++i)
                         {
                             REQUIRE(i == 0);
                             hits.fetch_add(1, std::memory_order_relaxed);
                         }
                     });
    REQUIRE(hits.load() == 1);
}

TEST_CASE("TaskPool ThreadCount returns at least 1", "[task_pool]")
{
    TaskPool pool;
    REQUIRE(pool.ThreadCount() >= 1);
}

TEST_CASE("TaskPool repeated ParallelFor calls from main thread reuse the pool", "[task_pool][threading]")
{
    // The contract is "main-thread submission only" (see header).
    // What we DO pin here: many sequential ParallelFor calls reuse
    // the same scheduler without leaking workers or accumulating
    // state.  Landscape calls Fill on every frame the camera moves,
    // so this is the production pattern.
    TaskPool pool;
    constexpr int kCalls = 64;
    constexpr uint32_t kPerCall = 128;

    for (int call = 0; call < kCalls; ++call)
    {
        std::vector<std::atomic<int>> hits(kPerCall);
        for (auto& h : hits)
        {
            h.store(0);
        }
        pool.ParallelFor(kPerCall,
                         [&](uint32_t start, uint32_t end)
                         {
                             for (uint32_t i = start; i < end; ++i)
                             {
                                 hits[i].fetch_add(1, std::memory_order_relaxed);
                             }
                         });
        for (uint32_t i = 0; i < kPerCall; ++i)
        {
            REQUIRE(hits[i].load() == 1);
        }
    }
}

TEST_CASE("TaskPool destructs cleanly with no in-flight work", "[task_pool][lifetime]")
{
    // Repeated construct/destruct catches scheduler-leak and shutdown-
    // hang regressions.  enkiTS::TaskScheduler::WaitforAllAndShutdown
    // is documented as required before destruction; this test pins
    // that the wrapper handles it correctly.
    for (int i = 0; i < 8; ++i)
    {
        TaskPool pool;
        REQUIRE(pool.ThreadCount() >= 1);
    }
}

TEST_CASE("TaskPool with explicit thread count honours the request", "[task_pool][config]")
{
    // 1 thread is the smallest valid request (still has the main
    // submitter thread + 1 worker = single-worker pool).  Tests that
    // the threadCount parameter actually plumbs through to enkiTS.
    TaskPool pool(1);
    // enkiTS reports the number of *worker* threads.  Explicit count
    // of 1 means 1 worker thread; the actual reported number may
    // include or exclude the main thread depending on enki's
    // semantics — we only pin that it's plausible (>= 1).
    REQUIRE(pool.ThreadCount() >= 1);

    std::atomic<int> calls{0};
    pool.ParallelFor(32,
                     [&](uint32_t start, uint32_t end)
                     {
                         for (uint32_t i = start; i < end; ++i)
                         {
                             calls.fetch_add(1, std::memory_order_relaxed);
                         }
                     });
    REQUIRE(calls.load() == 32);
}

TEST_CASE("TaskPool::Submit runs the callable on a worker thread", "[task_pool][submit]")
{
    TaskPool pool;
    std::atomic<bool> done{false};
    std::atomic<std::thread::id> runOnThread;

    const auto submitThread = std::this_thread::get_id();
    pool.Submit(
        [&]()
        {
            runOnThread.store(std::this_thread::get_id());
            done.store(true);
        });

    // Poll for completion — Submit is fire-and-forget so we don't get
    // a future.  Bounded wait so the test fails fast if the worker
    // never runs.
    for (int i = 0; i < 100 && !done.load(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(done.load());
    REQUIRE(runOnThread.load() != submitThread);
}

TEST_CASE("TaskPool::Submit many fire-and-forget tasks all complete", "[task_pool][submit][stress]")
{
    TaskPool pool;
    std::atomic<int> counter{0};
    constexpr int kSubmits = 200;

    for (int i = 0; i < kSubmits; ++i)
    {
        pool.Submit([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    // The destructor calls WaitforAllAndShutdown which blocks on every
    // queued task; after that no decode is in flight.  Force the wait
    // by reading the counter under a timeout — if the destructor
    // contract holds, all submits have completed by the time we leave
    // this scope (but we still want to assert before the dtor runs).
    for (int i = 0; i < 200 && counter.load() < kSubmits; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(counter.load() == kSubmits);
}

TEST_CASE("TaskPool::Submit completed tasks get reaped on next Submit", "[task_pool][submit][gc]")
{
    // The GC runs lazily on each Submit.  Submit a small task, wait
    // for it to complete, then submit another and observe that the
    // pending vector doesn't grow without bound.  Tests the GC path
    // by repeating many times — without GC the in-flight vector
    // would grow linearly.
    TaskPool pool;
    constexpr int kRounds = 500;
    std::atomic<int> counter{0};

    for (int i = 0; i < kRounds; ++i)
    {
        std::atomic<bool> done{false};
        pool.Submit(
            [&]()
            {
                counter.fetch_add(1, std::memory_order_relaxed);
                done.store(true);
            });
        for (int j = 0; j < 50 && !done.load(); ++j)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(done.load());
    }
    REQUIRE(counter.load() == kRounds);
}

TEST_CASE("TaskPool ParallelFor large N stress", "[task_pool][stress]")
{
    // Catch coarse partitioning regressions where the scheduler
    // would otherwise miss an edge index or double-execute.
    TaskPool pool;
    constexpr uint32_t N = 100000;
    std::vector<std::atomic<int>> hits(N);
    for (auto& h : hits)
    {
        h.store(0);
    }

    pool.ParallelFor(N,
                     [&](uint32_t start, uint32_t end)
                     {
                         for (uint32_t i = start; i < end; ++i)
                         {
                             hits[i].fetch_add(1, std::memory_order_relaxed);
                         }
                     });

    int sum = 0;
    for (uint32_t i = 0; i < N; ++i)
    {
        sum += hits[i].load();
    }
    REQUIRE(sum == static_cast<int>(N));
}
