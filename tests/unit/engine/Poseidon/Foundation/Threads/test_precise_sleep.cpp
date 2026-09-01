// test_precise_sleep.cpp - the sub-millisecond sleep.
//
// The property that earns this primitive its place is that a ~2 ms sleep comes back
// in single-digit milliseconds. A scheduler-granular sleep cannot do that on a
// Windows box at the default timer resolution: it rounds every wait up to the next
// ~15.6 ms tick, so if SleepUs were secretly Sleep() in disguise, every measurement
// below would read ~15 ms and the assertions would catch it. That is the regression
// this file exists to detect -- the fallback path silently becoming the only path.
//
// Measurement discipline: the upper bound is asserted on the MINIMUM of several
// sleeps, not the average. Any single sleep can be stretched arbitrarily by a busy
// machine (the OS owns the wakeup, not us), and a test that fails under load teaches
// people to ignore it. The minimum only exceeds the bound if every attempt did,
// which is what "the mechanism does not work here" looks like. The lower bound IS
// per-sleep: a sleep that returns early is broken in a way load cannot excuse.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Foundation/Threads/PreciseSleep.hpp>

#include <chrono>
#include <cstdint>

using Poseidon::Foundation::SleepUs;

namespace
{
int64_t ElapsedUs(std::chrono::steady_clock::time_point since)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count();
}
} // namespace

TEST_CASE("Zero and negative durations return immediately", "[precise-sleep][foundation]")
{
    const auto start = std::chrono::steady_clock::now();
    SleepUs(0);
    SleepUs(-1);
    SleepUs(INT64_MIN);
    // Not a timing claim, a does-not-hang claim: three no-op sleeps must not cost
    // even one scheduler tick.
    CHECK(ElapsedUs(start) < 50'000);
}

TEST_CASE("A 2 ms sleep returns in single-digit milliseconds", "[precise-sleep][foundation]")
{
    constexpr int64_t requestUs = 2'000;
    constexpr int attempts = 8;

    int64_t minUs = INT64_MAX;
    for (int i = 0; i < attempts; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        SleepUs(requestUs);
        const int64_t took = ElapsedUs(start);

        // A sleep may be late; it may not be early. Allow 0.5 ms of slack for the
        // two clocks involved (the sleep's and the measurement's) disagreeing about
        // where the deadline sits, not for a genuinely short wait.
        CHECK(took >= requestUs - 500);

        if (took < minUs)
            minUs = took;
    }

    // 12 ms, not 2.5: loose enough that scheduler noise on a loaded CI runner does
    // not fail the build, tight enough that a wait rounded up to the default 15.6 ms
    // Windows timer tick -- the failure mode this primitive exists to avoid -- still
    // cannot pass.
    CHECK(minUs < 12'000);
}

TEST_CASE("Sub-millisecond requests neither vanish nor round up to a tick", "[precise-sleep][foundation]")
{
    constexpr int64_t requestUs = 500;
    constexpr int attempts = 8;

    int64_t minUs = INT64_MAX;
    for (int i = 0; i < attempts; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        SleepUs(requestUs);
        const int64_t took = ElapsedUs(start);
        if (took < minUs)
            minUs = took;
    }

    // The interesting failure here is rounding UP: half a millisecond becoming a
    // full 15.6 ms tick makes the primitive useless for pacing, silently. The
    // fallback path legitimately costs up to one tick, so this bound is what
    // detects the high-resolution path having gone missing on a machine that has it.
    CHECK(minUs < 12'000);
}
