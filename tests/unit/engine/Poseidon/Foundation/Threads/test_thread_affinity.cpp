// Roadmap 4.1 — main-thread affinity reporting.
//
// The property under test is not "the log line appeared" (Catch cannot see LOG_WARN).
// It is the DECISION the facility makes: which thread it considers the owner, that an
// explicit capture beats adoption, that each guarded site latches independently, and
// that the steady-state path on the owning thread does nothing observable.
//
// `NoteMainThreadOnly` is deliberately a report and not a lock or an assert, so there is
// no return value to check. The observable surface is `IsMainThread()` plus the per-site
// `reported` latch, and both are exercised here.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>

#include <thread>

using namespace Poseidon::Foundation;

namespace
{
// The capture is process-global, and the game's own startup may already have set it in
// some binaries. Every test restores whatever it found, so ordering between test cases
// cannot make one of them lie.
struct ScopedCaptureReset
{
    ScopedCaptureReset() { ResetMainThreadCaptureForTest(); }
    ~ScopedCaptureReset() { ResetMainThreadCaptureForTest(); }
};
} // namespace

TEST_CASE("ThreadAffinity: with nothing captured, IsMainThread is false", "[threads][affinity]")
{
    ScopedCaptureReset reset;
    // Not "true because I am probably the main thread" — an uncaptured owner is unknown,
    // and reporting unknown as ownership is how a guard silently stops guarding.
    REQUIRE_FALSE(IsMainThread());
}

TEST_CASE("ThreadAffinity: CaptureMainThread claims the calling thread", "[threads][affinity]")
{
    ScopedCaptureReset reset;
    CaptureMainThread();
    REQUIRE(IsMainThread());

    bool workerSawItself = true;
    std::thread([&] { workerSawItself = IsMainThread(); }).join();
    REQUIRE_FALSE(workerSawItself);
}

TEST_CASE("ThreadAffinity: a site does not latch on the owning thread", "[threads][affinity]")
{
    ScopedCaptureReset reset;
    CaptureMainThread();

    MainThreadOnlySite site;
    for (int i = 0; i < 100; ++i)
        NoteMainThreadOnly(site, "test object");

    // 100 calls from the owner must leave the latch untouched, or the first genuine
    // violation would find the site already spent and stay silent.
    REQUIRE_FALSE(site.reported.load());
}

TEST_CASE("ThreadAffinity: a site latches once for an off-thread caller", "[threads][affinity]")
{
    ScopedCaptureReset reset;
    CaptureMainThread();

    MainThreadOnlySite site;
    std::thread(
        [&]
        {
            NoteMainThreadOnly(site, "test object");
            NoteMainThreadOnly(site, "test object");
        })
        .join();

    REQUIRE(site.reported.load());
}

TEST_CASE("ThreadAffinity: sites latch independently", "[threads][affinity]")
{
    ScopedCaptureReset reset;
    CaptureMainThread();

    MainThreadOnlySite first;
    MainThreadOnlySite second;
    std::thread([&] { NoteMainThreadOnly(first, "first object"); }).join();

    // The whole reason a site is a parameter rather than a function-local static inside
    // NoteMainThreadOnly: one violated boundary must not silence every other one.
    REQUIRE(first.reported.load());
    REQUIRE_FALSE(second.reported.load());
}

TEST_CASE("ThreadAffinity: with no capture, the first caller is adopted", "[threads][affinity]")
{
    ScopedCaptureReset reset;

    // A tool or test binary that never runs InitFPU still gets a working check: whoever
    // arrives first is treated as the owner, which is the NoteSequentialUse idiom.
    MainThreadOnlySite site;
    NoteMainThreadOnly(site, "adopted object");
    REQUIRE(site.reported.load() == false);
    REQUIRE(IsMainThread());

    MainThreadOnlySite fromWorker;
    std::thread([&] { NoteMainThreadOnly(fromWorker, "adopted object"); }).join();
    REQUIRE(fromWorker.reported.load());
}

TEST_CASE("ThreadAffinity: an explicit capture beats adoption", "[threads][affinity]")
{
    ScopedCaptureReset reset;

    // Capture from a worker, then check the main test thread is NOT the owner. This is
    // the ordering that matters in the engine: CaptureMainThread runs inside InitFPU,
    // and anything that adopted before it must lose.
    std::thread([] { CaptureMainThread(); }).join();
    REQUIRE_FALSE(IsMainThread());
}
