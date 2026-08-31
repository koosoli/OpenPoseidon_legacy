# SIM-807 — the preparer's worker conversion is not thread-safe, and the determinism harness found it (2026-08-31)

Roadmap **Phase 8.6, "Scheduler and determinism validation"**. Branch
`agent-preparer-determinism`, based on `safety/asset-obs-001` at `caba3bde`.
Direct follow-on to
[SIM-806](SIM-806-worker-count-determinism-harness-20260831.md), whose
"what is NOT covered" section named this target:

> **`ObjectStreamPreparer` is untested.** It runs on its own `std::thread` pool with a
> hard-coded worker count (2) … a genuine parallel authoritative-ish path with no
> determinism coverage; it needs game data to drive, which is why it is not here.

Harness: `tests/unit/engine/Poseidon/World/Model/test_preparer_determinism.cpp`
— 9 cases, tags `[determinism]` / `[preparer]` / `[ShapeAdapter]`, in
`PoseidonTests`. Eight of the nine need **no game data**.

**No game was launched for this document.** Another session holds the single
allowed instance and nothing here needs one.

---

## THE HEADLINE — and the one decision that is the owner's

**`ShapeAdapter::convertToLODShape` is not safe to run concurrently. Two threads
converting at the same time corrupt the process heap.** That is the shipping
arrangement: `WGR_OBJECT_STREAM_ASYNC_ADAPT` defaults **on**
(`LandSave.cpp:3580`) and `WGR_OBJECT_STREAM_ASYNC_WORKERS` defaults to **2**
(`ObjectStreamPrepare.cpp:60`), so the game runs two concurrent conversions
whenever a cold batch of streamed objects is admitted.

**The one-command mitigation is `WGR_OBJECT_STREAM_ASYNC_ADAPT=0`**, which
restores the synchronous adapt. Whether that becomes the *default* is a
performance decision — stage 3 is worth 3+ s of a traverse's cold admission —
and it is **not changed here**: it is the owner's call, and the owner is asleep.

Measured, same fixture, 64 conversions each run:

| threads | outcome |
|---|---|
| 1 | clean, every run |
| **2** | **heap corruption** — the shipping worker count |
| 4 | heap corruption |
| 8 | heap corruption |

The crash lands inside mimalloc (`_mi_page_malloc_zeroed`,
`_mi_heap_delayed_free_partial`, access violation reading `0xffffffff…`), at a
different site every run — the signature of a stray write, not of a bug in the
allocator. When it does not crash outright it produces a **bit-different
converted shape** on some jobs, which is how the harness first noticed: the
corpus arm failed 2 runs in 5 with "job index 4 of 12" before any crash was seen.

### What was ruled out

Each of these was a hypothesis that was *tested*, not argued:

| Hypothesis | Test | Result |
|---|---|---|
| It is the destruction, not the conversion | convert on workers, `delete` every shape on the main thread afterwards | **still crashes, 6/6** |
| It is the `RStringB` interning bank | give every selection ONE name and hold a live `RStringB` for it, so the bank does a single `GetOrAdd` and never `Unregister`s | **still crashes, 2/6** |
| It is the IR being mutated through a `const&` | `grep const_cast\|mutable` in `ShapeAdapter.cpp` and `Model.hpp` | no hits; the IR is genuinely read-only |
| It is lazy one-time init of some engine global | 64 **sequential** conversions, and a main-thread reference conversion before every threaded arm | clean; anything lazy is already initialised |
| It is `Ref<Texture>` refcounting through the shared bank tables | the synthetic fixture's textures resolve to `nullptr` in a test binary, and it still crashes | not texture refcounts |
| The allocators are unlocked | `RefCount` uses `__atomic_*`; `FastAlloc` and the `RStringB` bank both hold a `std::mutex`; `Buffer` is plain `new[]` | all already locked |

### What is implicated

Clearing the model's **named selections** makes the crash go away, and the crash
rate rises with the number of selections. So the stray write is in the
selection-building part of the conversion (`ShapeAdapter.cpp:1140-1180`,
`NamedSelection` construction + `Shape::AddNamedSel` + the following
`Shape::Compact()`) or in something it allocates alongside.

**The exact write is not identified, and this record does not claim it is.** The
tool that would name it is ASan; the Windows preset `win-x64-clang-san` **does
not configure in this tree** (`Configuring incomplete, errors occurred` →
`ninja: error: loading 'build.ninja'`). Fixing that preset, or running the
reproducer on the Linux ASan/TSan presets, is the obvious next step and is the
single highest-value follow-up from this session.

### The reproducer

No game data needed — a synthetic ODOL model with many uniquely named selections
is enough:

```
SIM807_CONCURRENT_ADAPT=1 PoseidonTests.exe "[race]"
SIM807_CONCURRENT_ADAPT=1 SIM807_RACE_THREADS=1 PoseidonTests.exe "[race]"   # clean
SIM807_CONCURRENT_ADAPT=1 SIM807_RACE_SELECTIONS=0 PoseidonTests.exe "[race]" # clean
```

### Why half the harness is opt-in

Every case that actually converts on more than one thread takes the process
down, and a `PoseidonTests` that segfaults would make the ctest run unusable for
every other worktree on this machine. Those arms are therefore behind
`SIM807_CONCURRENT_ADAPT=1` and run their **serial** arms by default. They are
written to pass, not deleted: the day the race is fixed, the gate comes out and
they become the worker-count determinism gate Phase 8.6 asked for.

This is a deliberate, named exception to "a test that cannot fail is worthless".
The default arms are not decorative — repeated serial conversion is what would
catch a conversion that depends on call order or on a static counter (§6 proves
it) — but they are **not** the worker-count gate, and this record should not be
read as though they were.

---

## 1. What is actually parallel here, and what is not

The preparer's worker loop (`ObjectStreamPrepare.cpp`, `Impl::WorkerMain`) does
exactly two things off the main thread:

| Stage | Call | Line | Covered here |
|---|---|---|---|
| parse | `ModelCache::LoadLooseFile(path, &error, &opened)` | `:255` | **failure classification only** |
| texture prepare | `ReadPAABlockChain` / `DecodePAABuffer` → `PreparedTextureStore` | `:288-401` | no |
| **convert** | `ShapeAdapter::convertToLODShape(*job.model, false, job.tables.get(), finishTail=false)` | `:198` | **yes — the fixture** |

Everything else in the cold-admission path is main-thread by construction and is
**outside** this harness by design, not by omission:

* **`BuildAdapterBankTables`** (`ShapeAdapter.cpp:423`) resolves `GlobLoadTexture`
  and `GTexMaterialBank` — the two global banks. `LandSave.cpp:3594` runs it on
  the main thread and hands the result to `SubmitConvert`. The harness builds the
  tables **once, on the test thread, and shares one table set across every arm** —
  exactly as production does, and so that "the arms were given different input"
  cannot explain either agreement or disagreement.
* **`FinishOdolAdapterTail`** (`ShapeAdapter.cpp:637`) — proxies,
  `OptimizeShapes`, `ScanShapes`, mass, convex components, `Reverse()`.
  `convertToLODShape` skips it entirely when `finishTail=false` (`:1507`), which
  is what a worker passes.
* **`CreateAdapterProxies`**, the `ShapeBank` install, `OptimizeOneShape`,
  vertex-buffer creation, `RegisterGpuModel`.

The parse is covered only for its **failure classification**, deliberately:
decoding a P3D is a pure function of its bytes, whereas the conversion is where
shared banks, the `RString` property table and the FP environment could diverge.

**The preparer's own worker count cannot be varied from a unit test**:
`ObjectStreamPreparer::WorkerCount()` is a read-once process-wide static fed by
an environment variable (`ObjectStreamPrepare.cpp:53`). That is why the
worker-count sweep drives the conversion **kernel** directly, mirroring the
worker's prologue (`ApplyMainFpEnvironment`, `reversed=false`, `finishTail=false`)
rather than driving the class.

## 2. What is compared

SIM-806's method, deliberately reused rather than reinvented. `HashConverted`
folds the **complete** `LODShapeWithShadow`:

* per LOD: every vertex position, normal, UV, clip flag and tangent frame as raw
  IEEE-754 **bits**; every face's arity, vertex indices, special flags and texture
  **index**; every section's special bits, texture index, special-material index
  and whether a `surfMat` is bound; every named selection's name; the shape's
  special bits, point count and bounding box;
* per shape: name, LOD count, every resolution, map type, special bits, mass,
  bounding sphere and centre, centre of mass, and all eight special-level indices
  `ScanShapes` resolves.

Bits, not values — `-0.0f == 0.0f` and `NaN != NaN`, so a value comparison is
simultaneously too loose and too tight for a strict gate. Textures and materials
are hashed by **index into the shape's own table** or as a bound/not-bound bit,
never by address: a pointer is allocator state (SIM-803 is an entire decision
record about ranking by one).

`HashComponents` is the same bits split per component ("LOD 3 positions", "LOD 1
sections", …) so a divergence is reported as a **place**, not just as two
different numbers — the analogue of SIM-806's "first divergent segment index".

## 3. The two anti-vacuity guards

* **`SIM-807 preparer fixture is non-degenerate`** measures the fixture: 5 LODs
  (four visual plus a geometry LOD at the 1e13 sentinel, `LodPurpose.hpp:70`),
  4 materials per LOD of which one carries an RVMAT path and one carries no
  texture at all, **1,445 vertices**, **1,710 faces** (172 triangles + 170 quads
  per LOD, so both arities are live), ≥20 sections, 2 named selections and 3 named
  properties per LOD.
* **All five LODs hash differently.** The hasher is not collapsing them.

The corpus arm's model, `packages/a3-compat/m/cargo/cargo_house_v1_f.p3d` (ODOL
revision 73), measures **19,843 vertices and 14,007 faces** across its LODs. It
is **not tagged `[GameData]`**: `OFPR_CATCH_TEST_SPEC` in
`tests/unit/engine/Poseidon/CMakeLists.txt` excludes `[GameData]` from the entire
ctest run whenever `packages/Remaster` is absent — and Remaster is absent on this
workstation, which has the corpora. Tagging it the obvious way would have made it
silently never run; [REN-GL33-002](REN-GL33-002-compat-smoke-tests-20260831.md)
§4 documents the same trap. It `SUCCEED`s with a message when the model is not
there, so CI is unaffected.

## 4. Three further findings

### 4a. The destructor leaked every parked converted shape — fixed

`Impl::convertedShapes` is a `std::vector<LODShapeWithShadow*>` of **raw**
pointers this class owns between a worker finishing and `TakeConverted`. `Reset`
deletes them (`ObjectStreamPrepare.cpp:532`), `DropStale` deletes them (`:644`),
`TakeConverted` transfers ownership — and `~ObjectStreamPreparer` did **none of
the three**. The vector's own destructor destroys the pointers, not the objects.

It is a **latent** leak, in SIM-806's sense: the shipping teardown calls `Reset`
on a world change, which frees them, so it is reachable only at final destruction
and on any path that destroys a preparer with work parked in it. It has no
symptom short of a leak checker — a shape that was never installed is one nothing
else references either, so there is no dangling pointer, no visual tell and no
log line. **The fix is five lines in the destructor**, after the join, which is
what makes it safe: no worker can be writing a slot.

The regression case is the tail of `SIM-807 cancellation mid-flight leaves no
slot behind`: it leaves one converted shape parked and lets the preparer go out
of scope.

**Honest limit:** found by reading, fixed by reasoning. It cannot be *measured*
here — a Catch2 assertion cannot observe a leak that leaves no dangling
reference, and the ASan preset does not configure (see the headline).

### 4b. `reversed` is inert on the worker side

The `reversed` control was written expecting the hash to **change** and measured
it **unchanged**. The reason is in the source: `reversed` is consumed only by
`FinishOdolAdapterTail` (`ShapeAdapter.cpp:772`, `shape->Reverse()`), which a
worker-side conversion skips. So the argument the preparer's worker passes
(`convertToLODShape(*job.model, false, …)`) is inert by construction, and
reversal is entirely the install's job.

Not a bug — every caller passes `false` today — but a trap: moving `Reverse()`
earlier in the conversion would silently make it a worker responsibility, and
the preparer hard-codes `false` with no way for a caller to say otherwise. The
control now pins the current arrangement as an equality, with the reasoning
beside it.

### 4c. `WGR_OBJECT_STREAM_ASYNC_READY` does not bound the converted set

The worker's wait predicate (`ObjectStreamPrepare.cpp:179`) is

```
stop || !convertQueue.empty() || (!queue.empty() && stats.ready < ReadyLimit())
```

so the ready bound gates the **parse** queue and nothing else. `convertQueue` is
drained unconditionally and **ahead of** parses, and a finished conversion parks
a whole `LODShapeWithShadow` in `convertedShapes[i]` **without touching
`stats.ready`**. A converted shape is strictly larger than the IR it came from.
Worse, the admit loop turns every `Ready` into a `SubmitConvert` the moment it
sees one (`LandSave.cpp:3595`), which keeps `stats.ready` near zero — so in the
shipping arrangement the pause the header describes rarely engages at all, and
the set it guards is not the set that grows.

Measured (`SIM-807 converted shapes are not bounded by the ready limit`, under
the concurrency gate): `ReadyLimit() + 8` conversions all reach `Converted` and
are all parked simultaneously, while `stats.ready` is **0** throughout.

**Nothing is changed for this.** The set is still bounded — by the model table,
and by `DropStale` once per recentre — so it is a documented property, not a
fault. It is pinned because the number a future author will reach for to cap
preparer memory is `ReadyLimit`, and on the current code that number does not do
it.

## 5. Proof the default (serial) arms can fail

Measured rather than asserted, in [REN-GL33-002](REN-GL33-002-compat-smoke-tests-20260831.md)
§4's style, and specifically chosen to be a **concurrency-only** defect so the
result means something: one line of `ShapeAdapter.cpp` was mutated to make the
conversion depend on which thread ran it —

```cpp
static const std::thread::id GAblationMain = std::this_thread::get_id();
shape->_mass = model.mass + (std::this_thread::get_id() == GAblationMain ? 0.0f : 1.0f);
```

| Run (mutation in place) | Result |
|---|---|
| the entire pre-existing suite (`~[preparer]`) | **3007 cases, 240,642 assertions, 0 failures** — passes clean |
| the SIM-807 harness, concurrency gate ON | **FAILS**, 4 of 8 cases (measured before the ninth case was split out) |

So: **a conversion that produced a different result on a worker thread than on
the main thread was invisible to every one of the project's 240,642 existing
assertions.** The mutation was reverted and the suite re-run green.

An earlier, cruder mutation (a plain `static int` counter folded into the mass,
which also changes the single-threaded result) *was* caught by the pre-existing
suite — 1 assertion in 1 case. That is recorded here so the claim above is read
narrowly: what nothing else catches is **thread-dependence**, not any change to
the mass.

## 6. Evidence

Build: `win-x64-clang-rwdi`, worktree `.claude/worktrees/agent-prep`, target
`PoseidonTests`, clean. Baseline measured at `caba3bde`.

| run | test cases | assertions | failures | skipped |
|---|---:|---:|---:|---:|
| baseline (`safety/asset-obs-001`, `caba3bde`) | 3008 | 240,655 | 0 | 20 |
| after this change (default, concurrency gate off) | 3017 | 240,863 | 0 | 20 |

+9 cases, +208 assertions. The `[preparer]` subset was run 6 consecutive times at the default
gating and passed every time (10 cases at that filter — the 9 new ones plus a
pre-existing `test_streaming_diag` case that shares the tag).

## 7. What is NOT covered

* **The worker-count gate is not on by default.** See the headline. Today this
  file proves *serial* repeat-determinism unconditionally and worker-count
  determinism only under `SIM807_CONCURRENT_ADAPT=1`, where it currently crashes.
* **The race is not root-caused and not fixed.** Localised to the
  named-selection part of the conversion, no further.
* **No ASan, no TSan.** The Windows ASan preset does not configure; the TSan
  preset is Linux-only (`linux-x64-clang-tsan`) and this session is on Windows.
  Both would likely name the write in minutes.
* **The parse stage is not hashed.** `ModelCache::LoadLooseFile` is covered only
  for its three-way outcome (`Ready` / `NotLoose` / `Failed`). Whether two
  workers parsing the *same* file concurrently produce identical IRs is untested —
  likely (the readers are header-only and allocate their own buffers) but "likely"
  is what this project measures instead of assuming.
* **The texture-prepare stage is not covered at all.** `PreparedTextureStore` is
  shared mutable state written from every worker, and `Put` / `PutDecoded` /
  `ShouldPrepare` race by design (a lost race counts as `texSkipped`). Whether
  the *store's* contents are worker-count-independent is a separate question.
* **`BuildAdapterBankTables` is not tested for thread safety, because it is not
  supposed to be thread-safe.** Nothing here would notice if a future change
  moved it to a worker.
* **The tail is only observed through `ScanShapes`.** It was *measured* that the
  tail is otherwise a no-op on a fixture whose textures do not resolve:
  `OptimizeShapes` sorts faces by texture and every texture in a unit-test binary
  is null, `CalculateMass` is skipped because the IR carries a mass, and
  `_viewDensity` / `_propertyClass` / `_convexComponents` are not reachable
  through the public API. That is why the fixture carries a geometry LOD —
  without one, the tail control cannot fail.
* **No proxies.** `finishTail=false` means `CreateAdapterProxies` never runs, so
  a fixture proxy would contribute nothing to the hashed output. The proxy path
  is the install's, and it is untested here.
* **The synthetic fixture is not a real model.** The kernel under test is the
  unmodified production conversion, but four of its five LODs are generated. The
  corpus arm answers that, and it is one model on one machine.
* **The FP-environment arm is thin.** `CaptureMainFpEnvironment()` runs in a test
  binary where nothing has called `InitFPU` and `ENGINE_CONFIG.enablePIII` is not
  in play, so the captured environment is the platform default. SIM-801 covers
  the capture/apply mechanism; this harness only checks the conversion agrees
  under it.
* **Nothing about the admit loop.** `Landscape::UpdateModernObjectResidency`
  decides when to `Request`, `Take`, `SubmitConvert` and `TakeConverted`; the
  state machine is tested here in isolation, the policy that drives it is not.
* **Nothing was verified in the game.** No capture, no deploy — the only engine
  change is a destructor freeing what it already owned.

## 8. Files

* `tests/unit/engine/Poseidon/World/Model/test_preparer_determinism.cpp` — new,
  9 cases.
* `tests/unit/engine/Poseidon/CMakeLists.txt` — registers it.
* `engine/Poseidon/World/Terrain/ObjectStreamPrepare.cpp` — the destructor fix
  (§4a).

No shader, no renderer, no data and no deploy.
