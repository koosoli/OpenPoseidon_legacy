// Roadmap 8.1 — worker-thread numerical environment.
//
// The property under test is not "the API sets some bits". It is: arithmetic that is
// SENSITIVE to the floating-point control state produces the same bits on a worker as on
// the main thread. So every test here perturbs a worker deliberately and then checks both
// the control word AND a denormal-sensitive computation.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Foundation/Platform/FpEnvironment.hpp>

#include <atomic>
#include <cfenv>
#include <cstdint>
#include <cstring>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#define TEST_FP_HAS_SSE 1
#include <xmmintrin.h>
#else
#define TEST_FP_HAS_SSE 0
#endif

using namespace Poseidon::Foundation;

namespace
{

// Restores this thread's FP control state on scope exit, so a test that deliberately
// corrupts the environment cannot leak it into the rest of the suite.
struct FpGuard
{
#if TEST_FP_HAS_SSE
    uint32_t mxcsr = _mm_getcsr();
#endif
    int rounding = std::fegetround();

    ~FpGuard()
    {
        std::fesetround(rounding);
#if TEST_FP_HAS_SSE
        _mm_setcsr(mxcsr);
#endif
        // The process-wide reference is global state; leave it agreeing with the restored
        // thread so a perturbing test cannot strand a later one on a bogus capture.
        if (MainFpEnvironmentCaptured())
        {
            CaptureMainFpEnvironment();
        }
    }
};

#if TEST_FP_HAS_SSE
constexpr uint32_t kFtzBit = 0x8000u; // MXCSR.FTZ
constexpr uint32_t kDazBit = 0x0040u; // MXCSR.DAZ
#endif

// A computation whose result depends on flush-to-zero. The product must land INSIDE the
// binary32 subnormal window -- between the smallest denormal (~1.4e-45) and the smallest
// normal (~1.18e-38) -- or it underflows to zero on its own and the test proves nothing.
// 1e-20f squared is 1e-40: with FTZ off that is a denormal, with FTZ on it is +0.
// Returned as raw bits so the comparison is exact rather than "close enough".
uint32_t DenormalSensitiveBits()
{
    volatile float tiny = 1e-20f;
    volatile float product = tiny * tiny;
    float value = product;
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// Put this thread into a state that is definitely NOT the platform default.
void PerturbFpEnvironment()
{
#if TEST_FP_HAS_SSE
    _mm_setcsr(_mm_getcsr() | kFtzBit | kDazBit);
#endif
    std::fesetround(FE_TOWARDZERO);
}

} // namespace

TEST_CASE("FpEnvironment: reading the current environment reports something valid", "[fp][threads]")
{
    const FpEnvironmentSnapshot now = ReadFpEnvironment();
    REQUIRE(now.valid);
    REQUIRE(FpEnvironmentsEquivalent(now, now));

    const FpEnvironmentSnapshot nothing;
    REQUIRE_FALSE(nothing.valid);
    // An invalid snapshot never compares equal — not even to itself.
    REQUIRE_FALSE(FpEnvironmentsEquivalent(nothing, nothing));
}

TEST_CASE("FpEnvironment: comparison ignores sticky status flags but not control bits", "[fp][threads]")
{
    FpEnvironmentSnapshot a = ReadFpEnvironment();
    FpEnvironmentSnapshot b = a;

    // Bits 0..5 are the sticky exception history. Two threads that did different work
    // legitimately differ here and must still count as the same environment.
    b.mxcsr = a.mxcsr ^ 0x3Fu;
    REQUIRE(FpEnvironmentsEquivalent(a, b));

#if TEST_FP_HAS_SSE
    // FTZ is a control bit and must not be ignored.
    b = a;
    b.mxcsr = a.mxcsr ^ kFtzBit;
    REQUIRE_FALSE(FpEnvironmentsEquivalent(a, b));
#endif

    b = a;
    b.roundingMode = a.roundingMode + 1;
    REQUIRE_FALSE(FpEnvironmentsEquivalent(a, b));
}

TEST_CASE("FpEnvironment: a perturbed worker is restored to the captured main state", "[fp][threads]")
{
    FpGuard guard;
    CaptureMainFpEnvironment();
    REQUIRE(MainFpEnvironmentCaptured());
    REQUIRE(FpEnvironmentMatchesMain());

    const FpEnvironmentSnapshot mainEnv = MainFpEnvironment();
    REQUIRE(mainEnv.valid);

    std::atomic<bool> matchedBeforeApply{true};
    std::atomic<bool> matchedAfterApply{false};
    std::atomic<bool> ensureReportedDrift{false};

    std::thread worker(
        [&]
        {
            PerturbFpEnvironment();
            matchedBeforeApply.store(FpEnvironmentMatchesMain());

            // EnsureFpEnvironmentMatchesMain reports that it had to correct the thread...
            ensureReportedDrift.store(EnsureFpEnvironmentMatchesMain());
            matchedAfterApply.store(FpEnvironmentMatchesMain());
        });
    worker.join();

    // The perturbation has to have been detectable, or the rest of the test proves nothing.
    REQUIRE_FALSE(matchedBeforeApply.load());
    REQUIRE(ensureReportedDrift.load());
    REQUIRE(matchedAfterApply.load());

    // The main thread is untouched by any of it.
    REQUIRE(FpEnvironmentMatchesMain());
}

TEST_CASE("FpEnvironment: Ensure is a no-op on a thread that already agrees", "[fp][threads]")
{
    FpGuard guard;
    CaptureMainFpEnvironment();

    std::atomic<bool> drifted{true};
    std::thread worker(
        [&]
        {
            ApplyMainFpEnvironment();
            drifted.store(EnsureFpEnvironmentMatchesMain()); // already conformed: no correction
        });
    worker.join();

    REQUIRE_FALSE(drifted.load());
}

TEST_CASE("FpEnvironment: denormal-sensitive arithmetic gives identical bits on a worker", "[fp][threads]")
{
    FpGuard guard;

    // Run the whole check twice: once with the main thread on the platform default, once
    // with the main thread deliberately in flush-to-zero (which is what
    // ENGINE_CONFIG.enablePIII does to the real game's main thread). The worker must
    // follow the main thread in BOTH directions, so a facility that merely forced one
    // fixed mode would fail the second pass.
    for (int pass = 0; pass < 2; ++pass)
    {
        if (pass == 1)
        {
            PerturbFpEnvironment();
        }

        CaptureMainFpEnvironment();
        const uint32_t mainBits = DenormalSensitiveBits();

        std::atomic<uint32_t> workerBitsUnconformed{0};
        std::atomic<uint32_t> workerBits{0};

        std::thread worker(
            [&]
            {
        // Start from a state that disagrees with the main thread whichever pass
        // this is. Note this must be derived from the CAPTURE, not from the
        // worker's own MXCSR: a fresh thread already holds the platform default,
        // so an XOR of its own bits would land on the captured state by accident
        // on the pass where the main thread is the perturbed one.
#if TEST_FP_HAS_SSE
                const uint32_t opposite = ~MainFpEnvironment().mxcsr & (kFtzBit | kDazBit);
                _mm_setcsr((_mm_getcsr() & ~(kFtzBit | kDazBit)) | opposite);
#endif
                workerBitsUnconformed.store(DenormalSensitiveBits());

                ApplyMainFpEnvironment();
                workerBits.store(DenormalSensitiveBits());
            });
        worker.join();

        INFO("pass " << pass);
        REQUIRE(workerBits.load() == mainBits);

#if TEST_FP_HAS_SSE
        // Guard against a vacuous pass: if the unconformed worker had already produced the
        // same bits, this test would be green no matter what the facility did. The two
        // must differ, which is exactly the hazard 8.1 exists to close.
        REQUIRE(workerBitsUnconformed.load() != mainBits);
#endif
    }

    // Leave the process reference matching the (restored) main thread. FpGuard runs after
    // this scope, so re-capture from the test's own teardown order is handled by the next
    // test's CaptureMainFpEnvironment call.
}

#if TEST_FP_HAS_SSE
TEST_CASE("FpEnvironment: the captured MXCSR round-trips exactly on its control bits", "[fp][threads]")
{
    FpGuard guard;

    PerturbFpEnvironment();
    CaptureMainFpEnvironment();
    const FpEnvironmentSnapshot captured = MainFpEnvironment();
    REQUIRE((captured.mxcsr & kFtzBit) != 0);
    REQUIRE(captured.roundingMode == FE_TOWARDZERO);

    std::atomic<uint32_t> workerMxcsr{0};
    std::atomic<int> workerRounding{0};
    std::thread worker(
        [&]
        {
            ApplyMainFpEnvironment();
            workerMxcsr.store(_mm_getcsr());
            workerRounding.store(std::fegetround());
        });
    worker.join();

    REQUIRE((workerMxcsr.load() & kMxcsrControlMask) == (captured.mxcsr & kMxcsrControlMask));
    REQUIRE(workerRounding.load() == FE_TOWARDZERO);
}
#endif
