#include <Poseidon/Foundation/Platform/FPUSetup.hpp>
#include <Poseidon/Foundation/Platform/FpEnvironment.hpp>
#include <Poseidon/Foundation/Threads/ThreadAffinity.hpp>
#include <Poseidon/Core/Config/EngineConfig.hpp>
#include <Poseidon/Dev/Debug/DebugTrap.hpp>

using namespace Poseidon::Dev;
#ifndef _WIN32
#include <cfenv>

#endif
extern void SetFlushToZero();

namespace Poseidon::Foundation
{

void InitFPU()
{
#ifdef _WIN32
    // x64: SSE math, no precision control — only rounding + exception masking.
    if (GDebugger.IsDebugger())
    {
        _control87(RC_NEAR | MCW_EM & ~0, MCW_EM | MCW_RC);
    }
    else
    {
        _control87(RC_NEAR | MCW_EM, MCW_EM | MCW_RC);
    }
#ifdef _KNI
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    if (GDebugger.IsDebugger())
    {
        PoseidonAssert(_MM_GET_EXCEPTION_MASK() == _MM_MASK_MASK);
        _MM_SET_EXCEPTION_MASK(_MM_MASK_MASK & ~(
                                                   //_MM_MASK_INVALID|
                                                   //_MM_MASK_DENORM|
                                                   //_MM_MASK_DIV_ZERO|
                                                   //_MM_MASK_OVERFLOW|_MM_MASK_UNDERFLOW|
                                                   //_MM_MASK_INEXACT|
                                                   0));
    }
    else
    {
        // disable all exceptions - should be default
        _MM_SET_EXCEPTION_MASK(_MM_MASK_MASK);
    }
#endif
    if (ENGINE_CONFIG.enablePIII)
    {
        SetFlushToZero();
    }
#else // !_WIN32
    fesetround(FE_TONEAREST);
#endif

    // Roadmap 8.1: the state the main thread has RIGHT NOW is the process's intended
    // numerical environment. Record it here, at the end of the only function that sets
    // it, so every engine-owned worker can be conformed to it. Threads created before
    // this point (the enkiTS pool is one — GameBase constructs it during command-line
    // parsing, well before InitializeEngineCore) are caught by the per-task
    // `EnsureFpEnvironmentMatchesMain()` in TaskPool rather than at thread start.
    CaptureMainFpEnvironment();

    // Roadmap 4.1: same thread, same moment, different question. The FP capture records
    // what the main thread's arithmetic looks like; this records WHICH THREAD it is, so
    // the objects the ownership table calls main-thread-only can say so at runtime
    // instead of only in a header comment. See Threads/ThreadAffinity.hpp.
    CaptureMainThread();
}

} // namespace Poseidon::Foundation
