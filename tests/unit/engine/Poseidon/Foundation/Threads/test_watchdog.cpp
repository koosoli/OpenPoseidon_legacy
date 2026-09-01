// test_watchdog.cpp - the stall detector.
//
// A diagnostic that silently does nothing is worse than no diagnostic, because it is read as
// "nothing is stalling". So the two properties worth pinning are the two directions of the
// answer: an overdue scope IS reported, and a scope that finished in time is NOT.
//
// The third test is the reason this is a port rather than a copy. The FP-GL original hands out
// ring slots with an unconditional `fetch_add & mask`, so once the counter laps a still-running
// slot, the older token's `Finish()` clears the newer scope's entry and a real stall goes unseen.
// Here each slot carries a generation and a token only clears the slot it was issued for. That
// is checked directly by lapping the ring on purpose.

#include <catch2/catch_test_macros.hpp>

#include <Poseidon/Foundation/Threads/WatchDog.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using Poseidon::WatchDog;
using Poseidon::WatchDogItem;

namespace
{
// Long enough that a loaded machine does not turn a pass into a failure, short enough that the
// suite does not notice. The sweep runs at this interval, so a report lands within ~2x of it.
constexpr auto Limit = std::chrono::milliseconds(40);

void WaitForSweeps(int count = 3)
{
    std::this_thread::sleep_for(Limit * count);
}
} // namespace

TEST_CASE("An overdue scope is reported", "[watchdog][foundation]")
{
    WatchDog dog(Limit);
    WatchDogItem item = dog.Add("stalled-scope");
    // Deliberately never finished while the sweep runs.
    WaitForSweeps();
    CHECK(dog.Reports() >= 1);
}

TEST_CASE("A scope that finishes in time is not reported", "[watchdog][foundation]")
{
    WatchDog dog(Limit);
    {
        WatchDogItem item = dog.Add("quick-scope");
        // Destructor finishes it immediately.
    }
    WaitForSweeps();
    CHECK(dog.Reports() == 0);
}

TEST_CASE("Finishing is idempotent and safe after the token is moved", "[watchdog][foundation]")
{
    WatchDog dog(Limit);
    WatchDogItem first = dog.Add("moved-scope");
    WatchDogItem second = std::move(first);
    first.Finish();  // moved-from: must be a no-op, not a double clear
    second.Finish(); // the real one
    second.Finish(); // idempotent
    WaitForSweeps();
    CHECK(dog.Reports() == 0);
}

TEST_CASE("A lapped slot is not cleared by the token that used to own it", "[watchdog][foundation]")
{
    // The bug an unguarded ring has. Hold a token, then lap the ring so the same slot is re-issued to a
    // scope that really does stall. If Finish() ignored generations, finishing the stale token
    // would clear the live entry and the stall would go unreported.
    WatchDog dog(Limit);

    WatchDogItem stale = dog.Add("stale-token");

    // Lap the ring exactly once so the next Add lands on the same slot. Each of these is finished
    // straight away, so none of them can be the thing that gets reported.
    for (size_t i = 0; i < WatchDog::RingSize - 1; ++i)
    {
        WatchDogItem transient = dog.Add("transient");
        transient.Finish();
    }

    // Same slot as `stale`, new generation, and this one is left to stall.
    WatchDogItem live = dog.Add("live-stall");

    // The stale token now points at `live`'s slot. Finishing it must NOT silence `live`.
    stale.Finish();

    WaitForSweeps();
    CHECK(dog.Reports() >= 1);
}

TEST_CASE("A path-named scope keeps the tail of the path", "[watchdog][foundation]")
{
    // The point of WatchScopeFor. Asset paths here share long directory prefixes, so a name
    // truncated from the FRONT is the same 31 characters for every scope and identifies
    // nothing. What has to survive the 31-character slot is the file at the end.
    //
    // There is no getter for a slot's name -- the log line is where it shows up -- so this
    // asserts the property the composition guarantees rather than the string: a scope whose
    // path differs only in its tail must still be reportable, and the helper must not
    // overrun the slot however long the inputs are.
    WatchDog dog(Limit);
    WatchDogItem a = Poseidon::WatchScopeFor("io.read", "ca\\data\\data3d\\very\\deep\\path\\alpha.paa");
    WatchDogItem b = Poseidon::WatchScopeFor("io.read", "ca\\data\\data3d\\very\\deep\\path\\beta.paa");
    // A label longer than the whole slot must not run past it either.
    WatchDogItem c = Poseidon::WatchScopeFor(std::string(200, 'L'), std::string(200, 'D'));
    WaitForSweeps();
    CHECK(dog.Reports() >= 3);
}

TEST_CASE("Without an armed watchdog, watching a scope costs nothing and is safe",
          "[watchdog][foundation]")
{
    // WatchScope is the call-site form: it must be usable unconditionally, including in a process
    // that never arms one (every tool binary).
    REQUIRE(WatchDog::Instance() == nullptr);
    WatchDogItem item = Poseidon::WatchScope("no-dog");
    item.Finish();
    SUCCEED("no crash, no watchdog");
}
