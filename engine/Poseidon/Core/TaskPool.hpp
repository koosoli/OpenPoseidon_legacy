// Process-wide worker pool, wrapping enkiTS::TaskScheduler.
//
// Construction is intentionally cheap so unit tests can stand up a
// pool per test case.  Production code uses one process-wide
// instance owned by `Application` and reached via `GetGlobalTaskPool()`.
//
//   TaskPool pool;
//   pool.ParallelFor(N, [&](uint32_t start, uint32_t end) {
//       for (uint32_t i = start; i < end; ++i) work(i);
//   });
//
// The callback receives [start, end) sub-ranges so the implementation
// can hoist per-thread state outside the inner loop.  Identical
// observable effect to `for (i in 0..N) work(i)`; the two are pinned
// equivalent by the test suite.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace enki
{
class TaskScheduler;
}

namespace Poseidon
{

class TaskPool
{
  public:
    // `threadCount == 0` lets enkiTS pick (one worker per logical core,
    // typically).  Otherwise the request is honoured directly.
    explicit TaskPool(uint32_t threadCount = 0);
    ~TaskPool();

    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;
    TaskPool(TaskPool&&) = delete;
    TaskPool& operator=(TaskPool&&) = delete;

    // Parallel-for over [0, count).  `work` is called with sub-ranges
    // [start, end); the union of all sub-ranges is exactly [0, count).
    // Blocks until every sub-range has completed.  No work scheduled
    // when `count == 0`.
    //
    // Must be called from the thread that constructed the TaskPool
    // (the "main thread" in production usage), or from inside a
    // running task on a TaskPool-worker thread.  Calling from an
    // arbitrary external std::thread will trip an enkiTS internal
    // assertion — those threads need `enki::RegisterExternalTaskThread`
    // first, which is intentionally not exposed at this API level
    // because no in-tree consumer needs it.
    void ParallelFor(uint32_t count, const std::function<void(uint32_t start, uint32_t end)>& work);

    // Fire-and-forget asynchronous submission.  The callable runs on
    // a worker thread; the caller does NOT block waiting for it.
    // TaskPool owns the task object until it completes; the GC of
    // completed tasks runs lazily on every Submit and once in the
    // destructor.
    //
    // Captures: the callable must own everything it needs.  In
    // particular, if it accesses an object whose lifetime is uncertain
    // (e.g. an asset that might be destroyed while the task is queued
    // or running), the callable should keep a strong reference inside
    // its closure.  TaskPool itself does NOT keep the callable's
    // captures alive beyond the callable's own copy.
    //
    // Submit may be called only from the thread that constructed the
    // TaskPool — same constraint as ParallelFor.
    void Submit(std::function<void()> work);

    // Number of worker threads in the underlying scheduler.  Always
    // >= 1 (enkiTS guarantees at least one worker on single-core hosts).
    uint32_t ThreadCount() const;

    // Raw enki handle for the one call site (landscape.cpp) that still
    // needs it directly.
    enki::TaskScheduler* RawScheduler();

  private:
    // enkiTS's submission API isn't safe for concurrent dispatch
    // from non-worker threads.  All in-tree call sites currently
    // dispatch from the main thread, but the lock makes the
    // contract robust to future consumers without measurable cost
    // (uncontended mutex acquire is sub-microsecond).
    std::mutex                           _submitMutex;
    std::unique_ptr<enki::TaskScheduler> _scheduler;

    // In-flight fire-and-forget submissions.  The vector owns each
    // task until enki reports it complete; GC happens lazily.  Stored
    // as void* to keep enkiTS out of the public header — actual type
    // is `Detail::FireAndForgetTask*` (see TaskPool.cpp).
    std::vector<void*>                   _pending;
    void                                 GcCompletedPending();
};

// Default cap on the auto-sized pool, so a very wide machine does not spawn a worker per
// core for work that stops scaling; override with --max-threads.
//
// It was 8, on the reasoning that a CWA-era game's parallel work saturates long before
// that. It no longer does (PERF, 2026-10-14, 16 logical cores, same binary): the native
// world load's model building runs on this pool, and a stock-Everon firefight's
// landscape and object passes split across it. With 16 against 8 -- native Everon
// SwitchLandscape 15.6 -> 14.4 s; perf_combat main thread land:obj 3.37 -> 2.43 ms and
// drw:land 4.34 -> 2.90 ms, with average and p95 frame unchanged (the GPU becomes the
// limit there). Machines with fewer logical cores are unaffected: the count never exceeds
// what the machine has.
inline constexpr uint32_t kDefaultMaxTaskThreads = 16;

// Resolve the TaskPool thread count (total enki threads, incl. the calling thread)
// from the --max-threads CLI value. `cliOverride`: >0 = explicit, 0 = uncapped (use
// all logical cores — returns 0 so enkiTS picks), <0 = auto (cap at
// kDefaultMaxTaskThreads, never more than `hardwareThreads`). `hardwareThreads == 0`
// (unknown) falls back to the cap. Pure, so it is unit-tested directly.
uint32_t ResolveTaskPoolThreadCount(int cliOverride, unsigned hardwareThreads);

// Process-wide instance.  Returns nullptr before `InitGlobalTaskPool`
// is called (e.g. very early bootstrap, or in tests that prefer their
// own instance).  Lifecycle is owned by `Application` —
// `InitGlobalTaskPool` is called once after CLI parsing and
// `ShutdownGlobalTaskPool` runs at process exit.
TaskPool* GetGlobalTaskPool();
void      InitGlobalTaskPool(uint32_t threadCount = 0);
void      ShutdownGlobalTaskPool();

} // namespace Poseidon
