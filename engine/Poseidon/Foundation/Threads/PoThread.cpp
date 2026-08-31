#include <Poseidon/Foundation/Threads/PoThread.hpp>

namespace Poseidon::Foundation
{

#ifdef _WIN32

bool poThreadCreate(ThreadId* id, long stackSize, THREAD_PROC_RETURN(THREAD_PROC_MODE* threadProc)(void*), void* arg)
{
    if (!threadProc)
    {
        return false;
    }
    if (stackSize < 0L)
    {
        stackSize = 0L;
    }
    DWORD thid; // thread id (unused)
    HANDLE tid = CreateThread(nullptr, stackSize, threadProc, arg, 0, &thid);
    if (id)
    {
        *id = tid;
    }
    return true;
}

bool poThreadJoin(ThreadId id, THREAD_PROC_RETURN* result)
{
    if (id == nullptr)
    {
        return false;
    }
    if (WaitForSingleObject(id, INFINITE) != WAIT_OBJECT_0)
    {
        return false;
    }
    if (!result)
    {
        return true;
    }
    return (GetExitCodeThread(id, result) != FALSE);
}

// `ThreadId` (= HANDLE on Win32) serves DOUBLE DUTY, and the two duties want different
// values from Windows. As an IDENTITY TOKEN -- which is all this function is for, and all
// its only callers in PoCritical use it for -- a thread ID is exactly right: unique per
// thread, stable, cheap, comparable. As a THING TO OPERATE ON, SetThreadPriority and
// friends need a real HANDLE, and a thread ID is not one.
//
// So do not feed this function's result to poSetPriority. That mistake was live in
// poSetMyPriority below until 2026-08-31; see the note there.
void poThreadId(ThreadId& id)
{
    id = reinterpret_cast<ThreadId>(static_cast<uintptr_t>(GetCurrentThreadId()));
}

const int PRIO[] = {
    THREAD_PRIORITY_LOWEST,       // -2 and below
    THREAD_PRIORITY_BELOW_NORMAL, // -1
    THREAD_PRIORITY_NORMAL,       //  0
    THREAD_PRIORITY_ABOVE_NORMAL, //  1
    THREAD_PRIORITY_HIGHEST       //  2 and more
};

bool poSetPriority(ThreadId id, int delta)
{
    if (delta < -2)
    {
        delta = -2;
    }
    else if (delta > 2)
    {
        delta = 2;
    }
    return (SetThreadPriority(id, PRIO[delta + 2]) != 0);
}

// FIXED 2026-08-31. This used to pass `GetCurrentThreadId()` -- a DWORD ID -- cast to a
// HANDLE. SetThreadPriority wants a handle, so the call failed and the function was a
// silent no-op on the ONLY platform the game ships on: every caller asking to be
// deprioritised stayed at normal priority and nothing said so, because the bool return was
// discarded at the call site.
//
// That mattered. The object-stream preparer workers ask for BELOW_NORMAL precisely so they
// can never take a core from the render thread (roadmap 11A, "streaming/decode cannot
// starve simulation"), and they had been running at normal priority the whole time. The
// preparer had already noticed and worked around it with a #ifdef _WIN32 direct call; the
// workaround is now removed, because a helper that lies is worse than no helper -- the next
// caller would not have known to work around it.
//
// GetCurrentThread() returns the pseudo-handle for the calling thread: always valid, needs
// no CloseHandle, and is what this always meant.
bool poSetMyPriority(int delta)
{
    return poSetPriority(GetCurrentThread(), delta);
}

#else

bool poThreadCreate(ThreadId* id, long stackSize, THREAD_PROC_RETURN(THREAD_PROC_MODE* threadProc)(void*), void* arg)
{
    if (!threadProc)
        return false;
    // stackSize is ignored on POSIX
    pthread_t tid;
    if (pthread_create(&tid, nullptr, threadProc, arg) != 0)
        return false;
    if (id)
        *id = tid;
    return true;
}

bool poThreadJoin(ThreadId id, THREAD_PROC_RETURN* result)
{
    return (pthread_join(id, result) == 0);
}

void poThreadId(ThreadId& id)
{
    id = (ThreadId)pthread_self();
}

bool poSetPriority(ThreadId id, int delta)
{
    // priority control not implemented on POSIX
    return true;
}

bool poSetMyPriority(int delta)
{
    return poSetPriority((ThreadId)pthread_self(), delta);
}

#endif

RefCountSafe::~RefCountSafe() = default;

double RefCountSafe::GetMemoryUsed() const
{
    return 0.0;
}

} // namespace Poseidon::Foundation
