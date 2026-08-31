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
// The bug was LATENT, and being precise about that matters. `git log -S` says the only
// Windows caller -- the object-stream preparer's workers, which ask for BELOW_NORMAL so
// they can never take a core from the render thread (roadmap 11A) -- carried a #ifdef
// _WIN32 direct SetThreadPriority workaround from the very commit that introduced them
// (daf92238). So no thread ever actually ran at the wrong priority. The other caller,
// NetPeer, uses poSetPriority with a real handle from poThreadCreate and was always fine.
//
// The fix is therefore about the TRAP, not about a live symptom: the next caller would
// have had no way to know the helper needed working around, and a helper that silently
// lies is worse than no helper. The workaround in the preparer is now removed, so there is
// one way to do this and it works.
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
