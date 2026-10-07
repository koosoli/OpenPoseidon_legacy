#pragma once

// Main-thread affinity reporting (roadmap Phase 4.1, ownership and authority).
//
// WHY THIS EXISTS. `design notes` says several engine-wide
// objects are MAIN THREAD ONLY: the model bank, the asset caches, the PBO file
// server's unlocked MRU. Until now every one of those rules lived in a header
// comment, which is a rule only for whoever reads the header. The audit that
// produced the table found that exactly this kind of comment had already gone
// stale against shipping code (`ObjectStreamPrepare.hpp` still says
// `ShapeAdapter::convertToLODShape` is main-thread-only; it has run on a worker,
// by default, since 2026-08-31) — so a comment is demonstrably not enough.
//
// WHAT THIS DOES. `CaptureMainThread()` records the main thread's id once at
// startup. `NoteMainThreadOnly(site)` compares the calling thread against it and
// emits a single `LOG_WARN` per site the first time they differ.
//
// WHAT THIS DELIBERATELY DOES NOT DO. It does not lock, and it does not assert.
// This is the same choice `RandomGenerator::NoteSequentialUse` made and for the
// same reason: adding a mutex to one of these objects would make the symptom go
// away while leaving the ordering dependency in place, and an assert turns a
// diagnosable warning into a crash in a release-ish build the owner is playing.
// The fix a report asks for is to move the call off the worker, not to serialise
// the callee.
//
// COST. One relaxed atomic load and an integer compare on the steady-state path,
// plus a second relaxed load per site once a report has fired. Cheap enough for a
// cache lookup; still not cheap enough for a per-vertex inner loop.
//
// IF IT WAS NEVER CAPTURED. `NoteMainThreadOnly` adopts the first caller as the
// owner (the `NoteSequentialUse` idiom), so the check still works in a unit-test
// or tool binary that never runs `InitFPU`.

#include <atomic>
#include <thread>

namespace Poseidon::Foundation
{

// Record this thread as the main thread. Called from `InitFPU()`, beside
// `CaptureMainFpEnvironment()`. Last call wins; calling it twice is harmless.
void CaptureMainThread();

// True if the calling thread is the recorded main thread. False when nothing has
// been captured AND nothing has adopted ownership yet.
bool IsMainThread();

// Per-site latch. Construct one `static` beside the call and pass it in, so each
// guarded site reports independently instead of the first one silencing the rest.
struct MainThreadOnlySite
{
    std::atomic<bool> reported{false};
};

// Report (once for this site) that a main-thread-only object was touched from
// another thread. `what` names the object and the rule, and goes verbatim into
// the log line — write it so a reader who has never seen this header knows what
// to do.
void NoteMainThreadOnly(MainThreadOnlySite& site, const char* what);

// Test seam: forget the captured owner so a test can drive the adoption path.
void ResetMainThreadCaptureForTest();

} // namespace Poseidon::Foundation

// One-liner for a guarded call site:
//
//     POSEIDON_MAIN_THREAD_ONLY("ShapeBank::New -- the model bank is main-thread "
//                               "only; workers use ModelCache::LoadLooseFile");
#define POSEIDON_MAIN_THREAD_ONLY(what)                                                                                \
    do                                                                                                                 \
    {                                                                                                                  \
        static ::Poseidon::Foundation::MainThreadOnlySite poseidonMainThreadOnlySite_;                                 \
        ::Poseidon::Foundation::NoteMainThreadOnly(poseidonMainThreadOnlySite_, (what));                               \
    } while (false)
