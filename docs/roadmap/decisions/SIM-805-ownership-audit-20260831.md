# SIM-805 — The ownership audit: a rule that shipping code contradicts, a version that is never checked, and an authority that is a bool (2026-08-31)

Roadmap **Phase 4.1, "Ownership and authority table"**. Branch `agent-ownership-table`,
based on `safety/asset-obs-001` @ `48a26cff`. Every citation in this record and in the
table was opened and read at that commit.

**No game was launched for this work** — two other sessions hold the single allowed
instance, and nothing here needs one. Everything below is source reading plus one unit
suite.

**Related:** [SIM-801](SIM-801-worker-fp-environment-20260831.md) (worker FP
environment), [SIM-803](SIM-803-stable-iteration-and-tie-breaking-20260831.md) (stable
iteration), [SIM-804](SIM-804-physics-integration-boundary-20260831.md) (physics
boundary), [SIM-806](SIM-806-worker-count-determinism-harness-20260831.md) and
[SIM-807](SIM-807-preparer-determinism-20260831.md) (what is genuinely parallel, and
the heap race in it), [REN-ID-001](REN-ID-001-stable-render-object-identity-20260831.md)
(identity), and the deliverable itself,
[`../ownership-authority.md`](../ownership-authority.md).

---

## Decision

Extend the ownership table from 13 rows to **36**, covering the whole spine Phase 4.1
names, and add a **Rule** column with three values — *stated*, *implicit*, *checked* —
so a reader can tell the difference between a rule the source writes down, a rule the
code merely happens to obey, and a rule the running program will notice being broken.

Then convert three rules from *stated* to *checked*, using a report rather than a lock
or an assert, and say why those three.

The table is the deliverable. This record is for the four things the audit found that a
table cell cannot hold.

---

## Finding 1 — the banner this document cited as its own authority is wrong

The previous version of `ownership-authority.md` cited
`World/Terrain/ObjectStreamPrepare.hpp:16-34` as the master statement of what a worker
thread may not touch. Lines 21–25 of that banner list
`ShapeAdapter::convertToLODShape` under **MAIN THREAD ONLY**, with four named reasons:
the global texture bank, `GTexMaterialBank.New`, the recursive `ShapeBank::New` for
proxies, and the `RStringB` property tables.

**Since 2026-08-31 the shipping default runs it on two worker threads.**

- The conversion executes on the preparer's own worker: `ObjectStreamPrepare.cpp:198`,
  inside `Impl::WorkerMain` (`:152`).
- It is on by default: `LandSave.cpp:3631-3636`, `WGR_OBJECT_STREAM_ASYNC_ADAPT`
  defaults on unless the variable is literally `"0"`.

This is not a bug in the code. The arrangement is sound and better evidenced than most
of this subsystem: the bank lookups the banner worries about were lifted out into
`AdapterBankTables`, resolved on the main thread and handed to the worker as
`shared_ptr<const>` (`LandSave.cpp:3646`), and the one real race behind it — a
non-atomic refcount in `CompactBuffer`, which is the buffer behind every `RString` and
therefore every interned `RStringB` — was found by SIM-807's harness and fixed in
`Foundation/Containers/CompactBuf.hpp:38-50`. The harness then measured 30/30
bit-identical conversions across 1/2/4/8/16 threads at 60 and 200 selections.

**The code is right and the rule is wrong**, and that is the exact failure this document
exists to prevent. A reader who trusts the header concludes a worker may not run the
adapter, and then either re-serialises a path worth 3+ s of cold admission per traverse,
or decides the shipping default is a mistake and reverts it. Neither is recoverable from
the header, because the header is what they read.

The row is marked **CONTESTED** in the table and cites both sides. Correcting the banner
is a doc change belonging to whoever owns that header; it was deliberately not made here,
because silently editing another subsystem's stated contract in an audit commit is how
the next reader loses the ability to see that the two ever disagreed.

**The general lesson, which is why this is finding 1:** a rule written only as a comment
decays silently and asymmetrically. The code moved; the comment could not. Three of the
four *checked* rows in the new table exist because of this finding.

## Finding 2 — the savegame version is written and never compared

`WorldSerializeVersion = 13` (`Core/SaveVersion.hpp:6`) appears in exactly three places
in the repository, and **all three are writes**: the constant itself and the two calls
that pass it into `ParamArchiveSave` (`World/WorldImpl.cpp:1823`, `:1874`, landing at
`IO/Serialization/ParamArchive.cpp:625-633`).

The reader (`ParamArchive.cpp:590-593` text, `:604-612` binary) has exactly one failure
mode: the `"version"` entry being **absent** (`LSNoEntry`). Any value that parses is
accepted, including one from the future.

The number's only consumption is per-field gating of the form
`if (_version < minVersion) return LSOK;` (`ParamArchive.cpp:132,157,181,205,229,265,…`,
macro `PARS_4_TO_3` at `ParamArchive.hpp:49-61`), plus hand-rolled branches such as
`AI/AICenterStats.cpp:250`, `AI/AISubgroup.cpp:559`, `World/Terrain/Visibility.cpp:104`.

That shape is correct for an **older** save and exactly backwards for a **newer** one:
every gate opens, and fields that do not exist in the file are read. The failure surfaces
later as a field-level `LSNoEntry` or — where a default is supplied — as silently wrong
data with no diagnostic at all.

Roadmap 4.1's closing sentence is "Persisted identities require an explicit versioned
serialization contract." The serialization is versioned. **The contract is not checked.**
A single comparison at the two read sites would close it. It is not done here because it
is a behaviour change to save compatibility and belongs to whoever owns that decision,
not to an audit.

## Finding 3 — multiplayer authority is a bool that ~150 call sites must remember

`NetworkObject::IsLocal()` (`Network/NetworkObject.hpp`) is the whole mechanism. It is
backed by a plain `bool _local` on vehicles (`World/Entities/Vehicles/Vehicle.hpp:285`)
and AI units (`AI/AIUnit.hpp:539`), flipped on ownership transfer
(`Network/NetworkClientOnMessage.cpp:402,454`).

Enforcement is an `if` at each site that chose to have one:
`AI/AIUnitImpl.cpp:80,225,827,885`, `World/Entities/Vehicles/Ground/Car.cpp:670,1395`,
`World/Entities/Vehicles/Air/Airplane.cpp:795,932,996,1079`, and so on. The nearest
thing to a chokepoint, `Network/NetworkClientActions.cpp:274-278`, only refuses to
**send** an update — the local mutation that produced it has already happened.

`Vehicle::SetTransform`, the damage setters and every other mutator are public and
callable on a remote object with no check. **Nothing in the type system distinguishes a
locally-owned handle from a remote one.** And `Object::IsLocal()` returns `true`
unconditionally (`World/Entities/Weapons/Dammage.cpp:712-720`), with `SetLocal` a
`Fail("Object is always local")`, so static scene objects are mutable everywhere by
construction.

This is a description, not a complaint. 4.1 asks the table to *identify* multiplayer
authority; the honest identification is "a convention", and writing it as anything
firmer would be the smoothing this audit was told not to do.

**One coupling worth carrying forward.** §4.1 warns that ephemeral render/audio/GPU
handles must not become savegame or network identities. That is **clean, and verified by
grep rather than assumed** — see the table's row 6.3. But the mirror image exists: a
static object's `NetworkId` is `NetworkId(STATIC_OBJECT, _id)`
(`Dammage.cpp:698-706`), derived from the same `_id` that `World::ResetIDs` renumbers on
**every save** (`World/WorldImpl.cpp:1664-1730`, called at `:1968-1969`). A save taken
mid-session can therefore change the value a live network identity was built from. It is
a Phase 11 item, not a Phase 4.1 one, but it was found here and would not be found by
looking at either subsystem alone.

## Finding 4 — a threading arm of the simulation is uncompilable dead code

`BACKGROUND_AI` gates a secondary AI thread in six places. It is `#define BACKGROUND_AI
0` in `World/WorldInit.cpp:90` and `World/WorldSetup.cpp:163`. In `World.cpp` —
`:1401`, `:1527`, `:1931`, the arm that would actually run the thread — **it is never
defined at all**, and the preprocessor's undefined-identifier rule evaluates it as 0.
Same outcome, different mechanism, and nothing states that the two files must agree.

The thread it guards does not exist. `SecondaryThread` is **forward-declared only**, at
`Core/Types.hpp:36`; the repository contains no definition. `new SecondaryThread`
(`World/WorldInit.cpp:119`) would fail to compile on an incomplete type the moment
anyone set the macro to 1.

So a reader who finds `#if BACKGROUND_AI` and reads it as a design to revive is reading
a design that was never here. Deleting the macro and the six arms is the right cleanup;
it is filed as work-list item 8 in the table rather than done here, because it touches
three files this branch has no other reason to open.

---

## What was enforced, and the three reasons for the three choices

The brief for this work said one cheap assertion at a real boundary is worth more than
ten pages of prose. Three were added. All three are **reports, not locks and not
asserts**, copying `RandomGenerator::NoteSequentialUse` deliberately: a mutex on one of
these objects would make the symptom vanish while leaving the ordering dependency in
place, and an assert converts a diagnosable warning into a crash in the build the owner
is playing.

**The facility.** `engine/Poseidon/Foundation/Threads/ThreadAffinity.{hpp,cpp}`.
`CaptureMainThread()` sits beside the existing `CaptureMainFpEnvironment()` at the end
of `InitFPU` (`Foundation/Platform/FPUSetup.cpp`) — the same thread, the same moment,
a different question: the FP capture records what the main thread's *arithmetic* looks
like, this records *which thread it is*. `POSEIDON_MAIN_THREAD_ONLY("…")` is one line at
a guarded call site and emits one `LOG_WARN` per site, ever. Steady-state cost on the
owning thread is one relaxed atomic load and an integer compare.

Two design points that are not obvious:

- **The latch is per site, not per process.** A function-local `static` inside
  `NoteMainThreadOnly` would mean the first violated boundary silences every other one.
  There is a test for exactly this.
- **With nothing captured, the first caller is adopted.** A tool or test binary that
  never runs `InitFPU` still gets a working check. An explicit `CaptureMainThread()`
  always beats adoption regardless of ordering; also tested.

### 1. `FileCache::Find` — `IO/FileServer.cpp`

The unlocked MRU array behind `GFileServer`. `Find` is the single funnel: `Use`, `Store`
and the eviction path all reach the cache through it (`FileServer.cpp:97,144,192`), so
one line covers every route by which a worker could read a PBO-packed file.

Chosen because its violation is **silent**. A worker racing `MoveToFront`'s array
shuffle corrupts memory and surfaces as a crash somewhere else entirely — which is the
exact shape of the bug SIM-807 spent a session chasing through mimalloc backtraces. The
rule has been in the header since 2026-08-30; this makes it audible instead of only
readable.

### 2. `ShapeBank::NewFromModel` — `Graphics/Rendering/Shape/ShapeDraw.cpp`

The entry point finding 1 is contested about, and the one the streamed-admission path
now calls **from the main thread with worker-produced IR**. The difference between that
and calling it *on* the worker is one function call in one branch. This is the guard that
keeps the current arrangement honest as Phase 5 and Phase 8 keep pushing work outward.

It also covers the `AssetCache` insert and the vertex-buffer creation beneath it, which
is how table row 2.8 gets a check without putting a logging dependency into a
header-only template.

`ShapeBank::New` deliberately is **not** guarded separately — it forwards to
`NewFromModel` (`ShapeDraw.cpp:598-601`), so guarding both would double the cost and
split the latch across two sites for one boundary.

### 3. The RNG guard now guards — `Random/randomGen.cpp:49`

The audit's most embarrassing find, and the reason it is worth writing down: **the
repository's worked example of a cheap enforced rule did not cover the case it was
written for.**

`NoteSequentialUse` was called from exactly one place, `SetSeed`
(`randomGen.hpp:72`). The header (`randomGen.hpp:28-48`) names four sequential entry
points — `SetSeed`, `RandomValue()`, `Gauss`, `PlusMinus` — and warns that they share
one mutable `_seed`. A worker that reached that stream would overwhelmingly **draw**
from it, not re-seed it, and `_valueTable(_seed++)` in `RandomValue()` is the
unsynchronised read-modify-write that makes the draw a race. The guard could not fire
for the scenario the comment above it describes.

`Gauss` (`randomGen.cpp:186`) and `PlusMinus` (`:203`) both route through
`RandomValue()`, so **one call** closes all three draw entry points. No new machinery.

## Verification

`tests/unit/engine/Poseidon/Foundation/Threads/test_thread_affinity.cpp` — 7 cases, 11
assertions, tags `[threads][affinity]`, in `PoseidonFoundationTests`, **no game data**.
It tests the decision the facility makes, not that a log line appeared (Catch cannot see
`LOG_WARN`): that an uncaptured owner reports as *unknown* rather than as *me*, that 100
calls from the owner leave the latch untouched, that an off-thread caller latches once,
that sites latch independently, that adoption works, and that an explicit capture beats
adoption whichever runs first.

Full `PoseidonFoundationTests` after the change: **491 cases, 164,414 assertions, all
passing** — including the existing `test_randomGen.cpp` suite, which is the regression
check that the added `NoteSequentialUse()` call in the hot draw path changed no value.

## What this pass did NOT do

- **No behaviour was changed.** The three guards log; nothing else moved.
- **The stale banner (finding 1) was not edited.** See finding 1 for why.
- **The version check (finding 2) was not added.** It is a save-compatibility decision.
- **`BACKGROUND_AI` (finding 4) was not deleted.** It touches three unrelated files.
- **`render-snapshot-design.md` §2 still says slice S4 is open.** It is closed —
  `UpdateModernObjectResidency` has exactly one caller and it is `World.cpp:1518`, on
  the simulation side. That document's own status header already says S1–S4 are done;
  only the bullet in its §2 body is stale. Noted in table row 2.2 rather than edited,
  for the same reason as finding 1: a companion document's inconsistency is evidence,
  and quietly resolving it removes the evidence.
