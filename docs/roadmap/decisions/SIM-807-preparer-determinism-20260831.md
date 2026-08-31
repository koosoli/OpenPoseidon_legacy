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

> **SUPERSEDED — the write was found later the same day. Skip to the part 2 appendix at
> the end of this document before acting on anything in this paragraph.** It is
> `CompactBuffer::_count`, a non-atomic reference count behind every interned string
> (`CompactBuf.hpp`), fixed in `8baf5d98`. In particular: ASan was **not** needed, the
> selection loop named below is the carrier and **not** the fault, and the two "next steps"
> in this paragraph are both wrong turns. Left in place rather than rewritten, because the
> reasoning that led here is worth seeing next to the answer.

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

The branch was rebased onto `safety/asset-obs-001` at `93a6433f` (10 commits
later) before landing, rebuilt and re-run: **3017 cases, 240,863 assertions, 0
failures, 20 skipped** — unchanged, so those ten commits added no cases and the
before/after above still reads straight.

The reproducer was re-run on the rebased tree: `SIM807_RACE_THREADS=4` crashed
2 runs of 3, `SIM807_RACE_THREADS=1` was clean 2 of 2. It is probabilistic, so
one clean run at 4 threads proves nothing and the case should be run several
times.

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

---

# SIM-807 part 2 — the write, found (2026-08-31)

Branch `agent-heap-race`. Measured at `safety/asset-obs-001` = `8c2235ab`.
Follow-on session, tasked with exactly one thing: find the stray write that part 1
localised to the named-selection loop but could not name.

**The fix landed as `8baf5d98`**, committed by the coordinating `asset-obs`
session ("Found by the heap-race agent; validated, integrated and deployed
here") while this session was still writing the record up. That commit's patch is
byte-identical to the one measured below — `git rebase` dropped this session's own
commit as "already upstream" — so the numbers here describe exactly the code that
shipped, and its `LandSave.cpp` comment supersedes the one drafted here. The three
commits rebased over in the meantime (`65be2fc6`, `fb41884f`, and `8baf5d98`
itself) touch `POST-DLSS-MODERNISATION-ROADMAP.md` and the fix's own three files,
so every measurement below still reads straight against the landed tree.

**It is found, it is fixed, and it was never in `ShapeAdapter.cpp` at all.**

## 9. The write

`engine/Poseidon/Foundation/Containers/CompactBuf.hpp:22-30`, as it stood before
this session:

```cpp
template <class Type>
class CompactBuffer
{
    // include RefCount functionality to avoid virtual functions table and delete
  private:
    mutable int _count;
  public:
    int AddRef() const { return ++_count; }
    int Release() const { int ret = --_count; if (ret == 0) { Delete(); } return ret; }
    int RefCounter() const { return _count; }
};
```

**The reference count on every `RString` buffer was a plain non-atomic `int`.**

`CompactBuffer<char>` is `String` (`RString.hpp:15`), and `RString` holds it as
`Ref<String>`. So this counter is the one behind *every* `RString` in the engine
and behind *every interned `RStringB`*. `Ref<Type>`'s copy constructor, copy
assignment, `Free()` and destructor (`Pointers.hpp:292-347`) call
`source->AddRef()` / `_ref->Release()` — which bind to **these** methods, not to
`RefCount`'s.

The failure is the textbook one. Two threads share an interned string. Both run
`++_count` on it; the read-modify-write is not atomic, one increment is lost;
later both run `--_count`; the count reaches 0 while a live reference is still
outstanding; `Release()` calls `Delete()`, which is `delete[] (char*)this`. The
buffer is freed under its owner. Everything after that is the allocator writing
into, and handing out, memory that another live object still points at — which is
why the crash lands inside mimalloc, at a **different site every run**, some
arbitrary time after the actual fault. Part 1 read that signature correctly as "a
stray write, not an allocator bug"; the stray write is the premature `delete[]`.

### Why part 1's "what was ruled out" table missed it

The row reads:

> The allocators are unlocked | `RefCount` uses `__atomic_*`; `FastAlloc` and the `RStringB` bank both hold a `std::mutex`; `Buffer` is plain `new[]` | all already locked

Every clause of that is true, and it still misses the counter, because
**`CompactBuffer` does not derive from `RefCount`**. Its own comment says why —
*"include RefCount functionality to avoid virtual functions table and delete"* —
it reimplements the interface to stay vtable-free. When `RefCount::_count` was
made atomic (`Pointers.hpp:230-247`, for the parallel terrain generation
double-free), `CompactBuffer` did not inherit that fix and nothing pointed at it.
The audit checked the base class that `RString` does not use.

The `RStringB` bank was checked too, and it is genuinely correct: `GetOrAdd`
returns **by value** under `_mtx` so a concurrent rehash cannot invalidate the
result. But the value it returns is an `RString`, and copying an `RString` is an
`AddRef` on the shared buffer — which happens **outside** that lock, in whichever
thread holds the copy, for the whole rest of the string's life. Locking the map
was never going to be enough; the bank's own comment ("Individual Get/Add locking
would not be enough") had the right instinct and stopped one level too early.

### Why it looked like the selection loop

Both of part 1's clues fall straight out of this, and neither implicates
`ShapeAdapter.cpp`:

* **The rate scales with named-selection count.** `NamedSelection` holds an
  `RStringB _name` (`Shape.hpp:126`). Each one is constructed
  (`Animation.cpp:306-312`, `_name = nameLow` → `RStringBT(RStringType)` →
  `GetOrAdd`) and then **copied again** by `AddNamedSel` → `_sel.Add(sel)` →
  `NamedSelection::DoConstruct(src)` → `_name = src._name`. Both threads convert
  the *same* model, so they intern the *same* names and land on the *same*
  `CompactBuffer`s. More selections is literally more concurrent unsynchronised
  `++`/`--` on shared counters. `SIM807_RACE_SELECTIONS=0` removes essentially
  all of that traffic, which is why clearing selections "fixed" it.
* **Pinning one name did not help (still crashed 2/6).** Recorded in part 1 as
  evidence *against* the bank. It is evidence *for* this: pinning removes the
  bank's map churn but concentrates every thread's refcount traffic onto one
  shared counter. It should, if anything, have made it worse — and part 1's own
  measurement of 2/6 is not distinguishable from the 3/6-ish baseline.

The named-selection loop was the *carrier*, not the fault. It is the densest
producer of shared interned strings in the conversion, so it is where a
process-wide bug in string refcounting surfaces first.

### The mechanism, demonstrated in isolation

Not inferred — measured. A 40-line standalone program (clang -O2, same machine)
with the two counter variants side by side, 4 threads × 100,000 balanced
`AddRef`/`Release` pairs, biased by 200,000 held references so a lost update
shows as drift instead of a free:

| variant | final count (5 runs, expected 200,001) |
|---|---|
| `++_count` / `--_count` | 200,003 · 200,003 · **199,997** · 200,004 · 200,007 — **0/5 correct** |
| `__atomic_add_fetch` / `__atomic_sub_fetch` | 200,001 ×5 — **5/5 correct** |

Note run 3: the plain counter drifts **downwards**. That is the direction that
frees a live buffer, and it is reachable in a few hundred thousand operations on
a quiet machine.

## 10. The fix

`CompactBuf.hpp` — `_count` moved to the GCC `__atomic` builtins, mirroring
`RefCount` exactly (`__ATOMIC_RELAXED` on the increment, `__ATOMIC_ACQ_REL` on the
decrement so the writes of every other holder are visible before the buffer is
freed, `__atomic_load_n` for the probe). Builtins rather than `std::atomic<int>`
for the same reason `RefCount` gives: no libatomic dependency. `CompactBuffer<char>`
is the only instantiation in the tree (grep: `RString.hpp:15` and nothing else),
so the blast radius is strings and only strings.

Two callers probe the count without holding a lock and are worth naming, because
both are now merely *imprecise* rather than *unsafe*:

* `RStringBT::Free()` (`RString.hpp:362-369`) reads `RefCounter() == 2` outside
  the bank lock to decide whether to `Unregister`. `Unregister` re-checks liveness
  under `_mtx` (`:476-488`), so a stale read costs at worst a string left interned
  in the bank — a bounded leak, not a free.
* `RString::MakeMutable()` reads `RefCounter() > 1`. Unreachable for interned
  strings: `RStringT` makes `Lower`/`Upper` private, and the one mutating caller
  in this path (`NamedSelection`'s constructor, `RString nameLow = name;
  nameLow.Lower();`) operates on a freshly created private buffer with exactly one
  reference.

Neither is changed. Both are noted so the next reader does not have to re-derive
that they are safe.

## 11. Measurements

Same reproducer as part 1, same machine, same `win-x64-clang-rwdi` preset,
worktree `.claude/worktrees/agent-heap`:

```
SIM807_CONCURRENT_ADAPT=1 PoseidonTests.exe "[race]"
```

| build | runs | non-zero exits |
|---|---:|---:|
| `8c2235ab`, before the fix | 12 | **9** (segfault, exit 139) |
| with the atomic `_count` | 12 | **0** |
| with the atomic `_count`, `SIM807_RACE_THREADS=8 SIM807_RACE_SELECTIONS=200` | 8 | **0** |

The stress arm is 8 threads and 200 selections per LOD — 3.3× the selection count
and 2× the threads of the default arm, i.e. well past the configuration that
crashed 9 times in 12 — and it is clean. Being probabilistic, 0/20 is strong
evidence and not a proof; the arm is cheap and should be re-run on any future
change to the string bank or to `Ref<>`.

Regression suites, both green on the fixed build:

| suite | cases | assertions | failures | skipped |
|---|---:|---:|---:|---:|
| `PoseidonTests` | 3017 | 240,863 | 0 | 20 |
| `PoseidonFoundationTests` | 484 | 163,839 | 0 | 0 |

`PoseidonTests` matches part 1 §6's post-change baseline exactly (3017 / 240,863 /
0 / 20), so the atomic counter costs no case and breaks none.

## 12. The regression test

`tests/unit/engine/Poseidon/Foundation/Strings/test_strings.cpp` — one new case,
`RString buffer refcount is atomic under concurrent AddRef/Release`, tags
`[strings][threads]`, in `PoseidonFoundationTests`.

It is written **not to crash** when the fix is reverted. Four threads run 100,000
balanced `AddRef`/`Release` pairs against one shared buffer that is pre-biased with
200,000 held references, so a lost update surfaces as a wrong final count rather
than as a premature free and a dead process. That matters: the whole reason half
of part 1's harness is behind `SIM807_CONCURRENT_ADAPT=1` is that a segfaulting
`PoseidonTests` makes ctest unusable for every other worktree on this machine.
This case does not have that problem and runs unconditionally.

Its teeth are measured, not assumed — the isolation experiment in §9 is the same
shape (same bias, same thread and iteration counts) and the non-atomic variant
failed 5 runs out of 5.

## 13. What this does NOT change

* **`WGR_OBJECT_STREAM_ASYNC_ADAPT` stays OFF by default.** It was turned off in
  `00aa5870` because this crash was in the owner's play path. Turning it back on
  is worth ~3 s of a traverse's cold admission and it is **the owner's decision**,
  not this session's. The comment at `LandSave.cpp:3569-3596` is updated to say the
  write was found rather than that it is still open, and the default is untouched.
* **No game was launched, nothing was deployed, no data was regenerated.** The
  change is one header and one test; the reproducer needs no game data. If the
  owner does re-enable stage 3, that is the point at which a deploy and a traverse
  are needed.
* **Part 1's other open items stand.** The parse stage is still unhashed, the
  texture-prepare stage is still uncovered, `PreparedTextureStore` still races by
  design, and `BuildAdapterBankTables` is still main-thread-only by contract. This
  session fixed one memory-safety bug; it did not audit the preparer.
* **This is not a claim that the conversion is now thread-safe.** It is a claim
  that the specific heap corruption SIM-807 reproduced is gone, over 20 runs
  including a 2×/3.3× stress arm. The bit-divergence part 1 saw ("job index 4 of
  12") is consistent with the same cause — a freed and reused name buffer hashes
  differently — but divergence-without-crash was not separately reproduced here,
  so that link is inference and is flagged as such.

## 14. A note for the next audit

The general lesson is narrow and worth stating plainly, because it is the reason
this took two sessions: **this codebase has two independent reference-count
implementations, and only one of them was audited.** `RefCount` (`Pointers.hpp`)
is atomic and carries a comment explaining why. `CompactBuffer` (`CompactBuf.hpp`)
reimplements `AddRef`/`Release`/`RefCounter` from scratch to avoid a vtable, and
was not. Anything reached through `Ref<>` binds to whichever of the two its type
actually provides. Grepping for `class RefCount` finds one of them; grepping for
`int AddRef() const` finds both. After this change they agree.

## 15. Files (part 2)

All three landed in `8baf5d98`:

* `engine/Poseidon/Foundation/Containers/CompactBuf.hpp` — the fix.
* `tests/unit/engine/Poseidon/Foundation/Strings/test_strings.cpp` — the
  regression case.
* `engine/Poseidon/World/Terrain/LandSave.cpp` — comment only, replacing the stale
  "the write has NOT been found" paragraph. **No behaviour change; the default is
  still off.** (Verified on this session's own draft of that hunk by diffing it and
  confirming no non-comment line changed; the landed wording is the integrating
  session's and says the same thing.)

This document is the only part of the work that did not land in that commit, which
is why it is a separate one.
