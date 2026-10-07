#pragma once

// Worker-thread numerical environment (roadmap Phase 8.1).
//
// WHY THIS EXISTS. A newly created thread does NOT inherit the creating thread's
// floating-point control state. On Windows/x64 every new thread starts with the
// *default* MXCSR (round-to-nearest, all exceptions masked, FTZ and DAZ OFF) and the
// default x87 control word, no matter what the main thread's state is. The main
// thread's state is not the default: `InitFPU()` calls `_control87` and, when
// `ENGINE_CONFIG.enablePIII` is set, `SetFlushToZero()` — which turns MXCSR.FTZ ON.
// A third-party DLL (graphics driver, NGX, audio backend) is also free to change the
// main thread's MXCSR behind our back at any point during initialisation.
//
// The consequence is concrete: the same geometry-conversion or simulation arithmetic
// run on a worker can produce different bits than it would on the main thread, because
// one of the two flushes denormals to zero and the other does not. That is a
// determinism hazard for anything whose result crosses back to the main thread — the
// streamed-object preparer's ShapeAdapter output today, gameplay jobs tomorrow.
//
// WHAT THIS DOES. `CaptureMainFpEnvironment()` records the process's intended FP
// environment once, on the main thread, after `InitFPU()` has finished. Every
// engine-owned worker thread then calls `ApplyMainFpEnvironment()` (or the cheaper
// `EnsureFpEnvironmentMatchesMain()`) so it computes in the same environment.
//
// WHAT THIS DOES NOT DO. It does not police threads the engine does not own — driver,
// runtime and middleware threads keep whatever they were given. It does not stop a
// library from changing the *main* thread's MXCSR after the capture; it only makes the
// workers agree with whatever was captured. See
// design notes
//
// Cost: a read is one `_mm_getcsr` (a few cycles). `EnsureFpEnvironmentMatchesMain()`
// is a read plus a compare, and only writes when the thread has actually drifted, so it
// is cheap enough to sit at the top of a task callback rather than only at thread start.

#include <cstdint>

namespace Poseidon::Foundation
{

// Control bits of MXCSR: DAZ (6), exception masks (7..12), rounding control (13..14),
// FTZ (15). Bits 0..5 are the sticky exception *status* flags — they are per-thread
// history, not configuration, and are deliberately excluded from every comparison.
inline constexpr uint32_t kMxcsrControlMask = 0xFFC0u;

struct FpEnvironmentSnapshot
{
    // Full MXCSR as read. Compare with `kMxcsrControlMask` applied, never raw.
    uint32_t mxcsr = 0;
    // Windows `_controlfp_s` word (`_MCW_DN | _MCW_EM | _MCW_RC` are the bits we keep).
    // Zero and unused off Windows.
    uint32_t controlFp = 0;
    // `<cfenv>` rounding mode (`FE_TONEAREST` etc.). Portable second witness; on x86 it
    // is redundant with the MXCSR rounding-control bits, on other ISAs it is all we get.
    int roundingMode = 0;
    // True when this build actually has SSE and `mxcsr` is meaningful.
    bool hasSse = false;
    // False on a default-constructed snapshot / before any capture.
    bool valid = false;
};

// Read this thread's current FP environment.
FpEnvironmentSnapshot ReadFpEnvironment();

// Main thread, once, at startup — call AFTER `InitFPU()`. Last call wins; calling it
// again from a worker would poison the reference, so don't.
void CaptureMainFpEnvironment();

// True once `CaptureMainFpEnvironment()` has run. All the Apply/Matches entry points
// are no-ops (returning true) before that, so early workers are not clamped to a
// half-initialised environment.
bool MainFpEnvironmentCaptured();

// The captured reference. `valid == false` if nothing has been captured yet.
FpEnvironmentSnapshot MainFpEnvironment();

// Worker-thread entry: force this thread into the captured environment.
void ApplyMainFpEnvironment();

// Cheap check, usable in a debug assert or a diagnostic readout. True when nothing has
// been captured yet.
bool FpEnvironmentMatchesMain();

// Check-then-apply. Returns true if the thread had drifted and was corrected, so a
// caller can count corrections instead of paying for an unconditional write.
bool EnsureFpEnvironmentMatchesMain();

// Two snapshots agree on every bit that governs arithmetic results (status flags and
// invalid snapshots excluded).
bool FpEnvironmentsEquivalent(const FpEnvironmentSnapshot& a, const FpEnvironmentSnapshot& b);

} // namespace Poseidon::Foundation
