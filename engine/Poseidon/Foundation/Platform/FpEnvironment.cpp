#include <Poseidon/Foundation/Platform/FpEnvironment.hpp>

#include <atomic>
#include <cfenv>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#define POSEIDON_FP_HAS_SSE 1
#include <xmmintrin.h>
#else
#define POSEIDON_FP_HAS_SSE 0
#endif

#ifdef _WIN32
#include <float.h>
#endif

namespace Poseidon::Foundation
{

namespace
{

// The reference environment. Written once from the main thread before any engine-owned
// worker starts reading it; `g_captured` is the release/acquire fence that publishes it.
FpEnvironmentSnapshot g_mainEnv;
std::atomic<bool> g_captured{false};

#ifdef _WIN32
// The `_controlfp_s` bits that change results: denormal handling, exception masks,
// rounding. Precision control (`_MCW_PC`) is x87-only and absent on x64.
constexpr unsigned int kControlFpMask = _MCW_DN | _MCW_EM | _MCW_RC;
#endif

} // namespace

FpEnvironmentSnapshot ReadFpEnvironment()
{
    FpEnvironmentSnapshot s;
    s.valid = true;
    s.roundingMode = std::fegetround();

#if POSEIDON_FP_HAS_SSE
    s.hasSse = true;
    s.mxcsr = _mm_getcsr();
#endif

#ifdef _WIN32
    unsigned int cw = 0;
    if (_controlfp_s(&cw, 0, 0) == 0)
    {
        s.controlFp = static_cast<uint32_t>(cw & kControlFpMask);
    }
#endif

    return s;
}

void CaptureMainFpEnvironment()
{
    g_mainEnv = ReadFpEnvironment();
    g_captured.store(true, std::memory_order_release);
}

bool MainFpEnvironmentCaptured()
{
    return g_captured.load(std::memory_order_acquire);
}

FpEnvironmentSnapshot MainFpEnvironment()
{
    if (!g_captured.load(std::memory_order_acquire))
    {
        return FpEnvironmentSnapshot{};
    }
    return g_mainEnv;
}

bool FpEnvironmentsEquivalent(const FpEnvironmentSnapshot& a, const FpEnvironmentSnapshot& b)
{
    if (!a.valid || !b.valid)
    {
        return false;
    }
    if (a.roundingMode != b.roundingMode)
    {
        return false;
    }
    if (a.hasSse != b.hasSse)
    {
        return false;
    }
    if (a.hasSse && ((a.mxcsr & kMxcsrControlMask) != (b.mxcsr & kMxcsrControlMask)))
    {
        return false;
    }
    return a.controlFp == b.controlFp;
}

void ApplyMainFpEnvironment()
{
    if (!g_captured.load(std::memory_order_acquire))
    {
        return; // nothing to conform to yet — leave the thread on the platform default
    }

    const FpEnvironmentSnapshot& want = g_mainEnv;

    std::fesetround(want.roundingMode);

#ifdef _WIN32
    // Do this BEFORE the MXCSR write. On x64 `_controlfp_s` is itself implemented on top
    // of MXCSR, so writing it second would undo the exact-bit restore below.
    unsigned int cw = 0;
    (void)_controlfp_s(&cw, static_cast<unsigned int>(want.controlFp), kControlFpMask);
#endif

#if POSEIDON_FP_HAS_SSE
    if (want.hasSse)
    {
        // Control bits from the capture; keep this thread's own sticky status flags.
        const uint32_t mine = _mm_getcsr();
        _mm_setcsr((mine & ~kMxcsrControlMask) | (want.mxcsr & kMxcsrControlMask));
    }
#endif
}

bool FpEnvironmentMatchesMain()
{
    if (!g_captured.load(std::memory_order_acquire))
    {
        return true;
    }
    return FpEnvironmentsEquivalent(ReadFpEnvironment(), g_mainEnv);
}

bool EnsureFpEnvironmentMatchesMain()
{
    if (FpEnvironmentMatchesMain())
    {
        return false;
    }
    ApplyMainFpEnvironment();
    return true;
}

} // namespace Poseidon::Foundation
