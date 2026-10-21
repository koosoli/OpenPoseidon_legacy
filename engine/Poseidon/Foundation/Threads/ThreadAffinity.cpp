#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>

#include <Poseidon/Foundation/Framework/Log.hpp>

#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

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
                 "See docs/roadmap/ownership-authority.md -- this is a report, not a lock, so the "
                 "race is still live; move the call off the worker rather than serialising the callee.",
                 what ? what : "(unnamed main-thread-only object)");
    }
}

void SetCurrentThreadName(const char* name)
{
    if (!name || !*name)
        return;
#ifdef _WIN32
    // Looked up rather than linked: SetThreadDescription is Windows 10 1607+.
    using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const SetDescription set = reinterpret_cast<SetDescription>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (!set)
        return;
    std::wstring wide;
    for (const char* c = name; *c; ++c)
        wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
    set(GetCurrentThread(), wide.c_str());
#else
    char shortName[16] = {}; // Linux limits names to 15 characters
    for (int i = 0; i < 15 && name[i]; ++i)
        shortName[i] = name[i];
    pthread_setname_np(pthread_self(), shortName);
#endif
}

void ResetMainThreadCaptureForTest()
{
    g_mainThread.store(std::thread::id(), std::memory_order_relaxed);
}

} // namespace Poseidon::Foundation
