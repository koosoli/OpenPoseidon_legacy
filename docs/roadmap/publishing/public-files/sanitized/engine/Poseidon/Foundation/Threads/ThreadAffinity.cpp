#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>

namespace Poseidon::Foundation
{

namespace
{
// Default-constructed means "nobody has claimed it yet". `CaptureMainThread` stores
// unconditionally; `NoteMainThreadOnly` only adopts when it is still unclaimed, so an
// explicit capture always wins over adoption no matter which runs first.
std::atomic<std::thread::id> g_mainThread{std::thread::id()};
} // namespace

void CaptureMainThread()
{
    g_mainThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
}

bool IsMainThread()
{
    const std::thread::id owner = g_mainThread.load(std::memory_order_relaxed);
    if (owner == std::thread::id())
        return false; // nothing captured, nothing adopted
    return owner == std::this_thread::get_id();
}

void NoteMainThreadOnly(MainThreadOnlySite& site, const char* what)
{
    const std::thread::id self = std::this_thread::get_id();
    std::thread::id      expected{};
    if (g_mainThread.compare_exchange_strong(expected, self, std::memory_order_relaxed))
        return; // first caller in a binary that never captured: adopt it
    if (expected == self)
        return; // the owner
    if (site.reported.load(std::memory_order_relaxed))
        return;
    if (!site.reported.exchange(true, std::memory_order_relaxed))
    {
        // spdlog formatting: `{}`, not printf's `%s`.
        LOG_WARN(Core,
                 "Thread affinity: {} was touched from a thread that is not the main thread. "
                 "See design notes -- this is a report, not a lock, so the "
                 "race is still live; move the call off the worker rather than serialising the callee.",
                 what ? what : "(unnamed main-thread-only object)");
    }
}

void ResetMainThreadCaptureForTest()
{
    g_mainThread.store(std::thread::id(), std::memory_order_relaxed);
}

} // namespace Poseidon::Foundation
