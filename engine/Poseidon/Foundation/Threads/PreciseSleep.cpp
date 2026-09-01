// SPDX-License-Identifier: AGPL-3.0-or-later
#include <Poseidon/Foundation/Threads/PreciseSleep.hpp>

#ifdef _WIN32
#include <Poseidon/Foundation/Common/Win.h>
#else
#include <cerrno>
#include <ctime>
#endif

namespace Poseidon::Foundation
{

#ifdef _WIN32

// Older SDKs do not name the flag; the kernel either honours the bit or rejects it,
// and rejection is handled below by retrying without it.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace
{

// One timer per thread, created on first use and closed at thread exit. Creating a
// kernel object per sleep would double the syscall count of a caller that sleeps
// every frame, and a single shared timer cannot be waited on concurrently -- two
// threads setting different due times on it would wake each other early.
struct ThreadSleepTimer
{
    HANDLE handle;

    ThreadSleepTimer() noexcept
        : handle(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS))
    {
        // The high-resolution flag needs Windows 10 1803; a kernel that predates it
        // fails the create with the flag set but accepts the plain form, which is
        // still correct -- just quantized to the timer interrupt like Sleep().
        if (!handle)
            handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }

    ~ThreadSleepTimer()
    {
        if (handle)
            CloseHandle(handle);
    }
};

} // namespace

void SleepUs(int64_t us) noexcept
{
    if (us <= 0)
        return;

    thread_local ThreadSleepTimer timer;
    if (timer.handle)
    {
        LARGE_INTEGER due;
        due.QuadPart = -(us * 10); // negative = relative, in 100 ns units
        if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE) &&
            WaitForSingleObject(timer.handle, INFINITE) == WAIT_OBJECT_0)
        {
            return;
        }
    }

    // No usable timer: degrade to the scheduler-granular sleep rather than not
    // waiting at all. Round UP so the degraded path never waits less than asked --
    // a pacer that undersleeps busy-loops, one that oversleeps merely paces coarser.
    ::Sleep(static_cast<DWORD>((us + 999) / 1000));
}

#else

void SleepUs(int64_t us) noexcept
{
    if (us <= 0)
        return;

    timespec ts;
    ts.tv_sec = static_cast<time_t>(us / 1000000);
    ts.tv_nsec = static_cast<long>((us % 1000000) * 1000);
    // nanosleep writes the remaining time back on EINTR, so resuming with the same
    // struct continues the wait instead of restarting it -- a signal storm cannot
    // stretch the sleep, and a single signal cannot cut it short.
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
    {
    }
}

#endif

} // namespace Poseidon::Foundation
