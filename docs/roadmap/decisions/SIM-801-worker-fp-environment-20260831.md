# SIM-801 — Worker threads did not share the main thread's FP environment (2026-08-31)

Roadmap **Phase 8.1, "Worker-thread numerical environment"**. Measured and implemented at
`6d00e70b` (trunk `consolidated-v2`).

## Decision

Capture the process's intended floating-point environment once on the main thread, at the
end of `InitFPU()` and at every other site that mutates main-thread FP state, and conform
every **engine-owned** worker thread to that capture before it computes anything whose
result crosses back to the main thread.

The facility is `engine/Poseidon/Foundation/Platform/FpEnvironment.{hpp,cpp}`. It is
deliberately *not* a policy — it does not decide what the environment should be. It copies
whatever the main thread ended up with, so a future change to `InitFPU`, or a decision to
turn `ENGINE_CONFIG.enablePIII` on or off, needs no edit here.

## What the audit found

No thread creation site in the tree set FP state. Not one.

| # | Thread | Created at | FP handling before | FP handling now |
| --- | --- | --- | --- | --- |
| T1 | Main / render | OS | `InitFPU()` sets rounding + exception masks; `SetFlushToZero()` sets MXCSR.FTZ when `ENGINE_CONFIG.enablePIII` | unchanged, plus `CaptureMainFpEnvironment()` |
| T2 | enkiTS TaskPool workers | `Core/TaskPool.cpp` ctor, from `GameBase.cpp:99` | none — platform default MXCSR | `profilerCallbacks.threadStart` hook **and** a per-task `EnsureFpEnvironmentMatchesMain()` in both task adapters |
| T3 | ObjectStreamPreparer workers | `World/Terrain/ObjectStreamPrepare.cpp` (`WorkerMain`) | none | `ApplyMainFpEnvironment()` at thread entry |
| T4 | Audio stream pump | `PoseidonOpenAL/SoundSystemOAL.cpp:185` | none | unchanged — see "not covered" |
| T5 | Legacy UDP peer listen/send | `Network/Legacy/NetPeer.cpp` | none | unchanged |
| T8a | `WatchDog::Sweep` | `Foundation/Threads/WatchDog.cpp:38` | none | unchanged |
| T8b | `DownloadWorker` | `Core/DownloadWorker.cpp:121` | none | unchanged |
| T8c | `MasterServerBrowser` worker | `Network/MasterServerBrowser.cpp:202` | none | unchanged |
| T8d | `NetworkMisc` download threads | `Network/NetworkMisc.cpp:283,300,318,325` | none | unchanged |
| T8e | `DebugTrap` helper | `Dev/Debug/DebugTrap.cpp:75` | none | unchanged |
| T8f | Workshop / Studio scan threads | `apps/tools/Studio/StudioApp.cpp:553,724,791` | none | unchanged (tool, not the game) |
| — | `poThreadCreate` (generic helper) | `Foundation/Threads/PoThread.cpp:19,83` | none | unchanged — it takes a caller-supplied `threadProc` and has no wrapper to inject into |

The only code in the whole repository that touches FP control state is:

- `Foundation/Platform/FPUSetup.cpp` — `_control87(RC_NEAR | MCW_EM, …)` on Windows,
  `fesetround(FE_TONEAREST)` elsewhere;
- `Foundation/Math/V3QuadsP3.cpp` — `SetFlushToZero()`, i.e. `_MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON)`;
- three call sites of that function: `FPUSetup.cpp`, `InitBridge.cpp` (`ApplyConfigurationFlags`)
  and `GameApplication.cpp` (`InitializeSubsystems`), all gated on `ENGINE_CONFIG.enablePIII`.

Nothing reads MXCSR back, nothing asserted on it, and nothing propagated it.

## Why it mattered

On Windows/x64 a new thread starts with the **default** MXCSR: round-to-nearest, all
exceptions masked, FTZ and DAZ **off**. It does not inherit the creating thread's. With
`enablePIII` set, the main thread runs with FTZ **on**. That is a bit-level divergence, not
a theoretical one, and the unit test pins it: the same denormal-producing multiply gives
`0x00000000` on a flush-to-zero thread and a subnormal on one without it.

The exposure today is T3. Since 2026-08-30/31 the streamed-object preparer runs full
`ShapeAdapter` geometry conversion on its own workers and the main thread installs the
result verbatim. Phase 8's simulation jobs will widen it considerably.

## The ordering trap

`GameBase` constructs the process-wide `TaskPool` during **command-line parsing**
(`GameBase.cpp:99`), which runs well before `InitializeEngineCore()` reaches `InitFPU()`.
So enkiTS's worker threads exist before there is anything to capture, and the
`threadStart` hook — the obvious and documented place to put this — fires while the capture
is still empty.

Rather than reorder startup (`InitFPU` depends on config that is not read yet), the two
enkiTS task adapters call `EnsureFpEnvironmentMatchesMain()` at the top of `ExecuteRange`.
That is a `_mm_getcsr` and a compare against the captured control bits; it writes only when
the thread has actually drifted. The `threadStart` hook is still wired, because it covers
pools created after startup and threads that never run through those adapters.

## What is enforced

- Rounding mode (`<cfenv>` `fegetround`/`fesetround`, portable second witness).
- MXCSR control bits — DAZ (6), exception masks (7–12), rounding control (13–14), FTZ (15),
  i.e. mask `0xFFC0`.
- Windows `_controlfp_s` under `_MCW_DN | _MCW_EM | _MCW_RC`. Applied **before** the MXCSR
  write, because on x64 `_controlfp_s` is itself implemented on MXCSR and would otherwise
  undo the exact-bit restore.

MXCSR bits 0–5 are the **sticky exception status flags**. They are per-thread history, not
configuration; two threads that did different work legitimately differ there and are still
the same environment. They are excluded from every comparison and preserved on apply.

## What is NOT covered

- **Third-party threads.** Driver, wgpu/Rust, OpenAL, NGX and Windows thread-pool threads
  keep whatever they were given. We do not own their entry points.
- **A library changing the MAIN thread's MXCSR after the capture.** The workers would then
  faithfully follow a stale reference. `FpEnvironmentMatchesMain()` exists so a diagnostic
  or debug assert can catch that on the main thread; nothing calls it that way yet.
- **T4/T5/T8.** Audio, networking and the auxiliary threads are untouched. None of them
  produces a value the simulation observes, so conforming them would be noise. If any of
  them ever starts feeding gameplay state, it joins the table above.
- **`poThreadCreate`.** The generic Foundation helper hands control straight to a
  caller-supplied `threadProc`; there is no wrapper to inject the apply into without
  changing its signature. Callers that matter call the facility themselves.
- **Non-x86.** On an ISA without SSE the facility degrades to rounding mode only. There is
  no ARM build of this engine today.

## Tests

`tests/unit/engine/Poseidon/Foundation/Platform/test_fp_environment.cpp`, tag `[fp]`,
6 cases / 23 assertions, in `PoseidonFoundationTests`.

The important one is `denormal-sensitive arithmetic gives identical bits on a worker`. It
runs **twice**: once with the main thread on the platform default, once with the main
thread deliberately in flush-to-zero — because a facility that merely forced one fixed mode
would pass the first pass and fail the second. Each pass also asserts that the
*unconformed* worker produced **different** bits, so the test cannot go green vacuously.

That guard earned its keep twice while writing this:

1. The first draft squared `1e-30f`. That is `1e-60`, far below the smallest binary32
   denormal (~`1.4e-45`), so it underflows to zero whatever FTZ says and the test proved
   nothing. The product has to land inside the subnormal window; `1e-20f` squared is
   `1e-40`, which does.
2. The second draft perturbed the worker by XOR-ing *its own* MXCSR. A fresh thread already
   holds the platform default, so on the pass where the *main* thread is the perturbed one
   the XOR landed exactly on the captured state by accident. The perturbation must be
   derived from the capture, not from the worker.

## Suite status at this commit

- `PoseidonFoundationTests`: 481 cases / 190,188 assertions. The `[fp]` cases pass. Three
  cases in `Framework/test_potime.cpp` fail on a loaded machine — they assert a `SLEEP_MS(10)`
  lands within 50 ms and measured 97.6 ms. Flaky, load-dependent, pre-existing; the failing
  count varies run to run (3, 3, 2 across three consecutive runs).
- `PoseidonTests`: 2932 cases / 234,628 assertions, 8 cases failing. **All eight reproduce
  in the main checkout's independently built binary from 2026-08-29**, i.e. they predate
  this work. Five are missing game data (`packages/Remaster` holds no stringtable CSV or
  `dtaExt` HTML on this box); one is `test_dev_panel_search` reporting raw `ImGui::` calls
  in `DebugOverlay.cpp`; one is `test_mp_version_message_localization`; one is
  `test_graphics_page` expecting AA row 11 to default to 0 and getting 2 — that last one
  fails in isolation too and looks like a genuine trunk defect, filed here as an
  observation, not touched.

## Latent hazards noticed, out of scope

- **`ENGINE_CONFIG.enablePIII` is applied from three different places** at three different
  points in startup (`InitFPU`, `ApplyConfigurationFlags`, `InitializeSubsystems`). All
  three now re-capture, so the reference is correct regardless of order, but one FP-state
  setter with one call site would be better than three.
- **Nothing verifies the main thread's MXCSR after third-party initialisation.** A driver
  or NGX DLL flipping FTZ during device creation would silently change the whole game's
  arithmetic and this facility would dutifully propagate it. A `FpEnvironmentMatchesMain()`
  check on the main thread once per frame in dev builds would catch it for near-zero cost.
- **`RangeTask` holds `const std::function<…>&`** — a borrowed reference into the caller's
  frame. Correct today because `ParallelFor` blocks, but it is one refactor away from a
  dangling read. Unrelated to 8.1.
- Phase 8.2 (random-number ownership) is the next real determinism gate and is untouched.
