# SIM-806 — Terrain generation is worker-count-deterministic, and the harness found a different bug (2026-08-31)

Roadmap **Phase 8.6, "Scheduler and determinism validation"**. Branch `agent-determinism`,
based on `safety/asset-obs-001`; the baseline suite counts below were measured at `e1b1e354` and the
work was rebased onto `0cadd67e` before landing (the branch moved three times during the session).
**Related:** [SIM-801](SIM-801-worker-fp-environment-20260831.md) (worker FP environment),
SIM-803 (stable iteration and tie-breaking — its code is in this base at `c3f202de`).

## Decision

Build the worker-count determinism check as a **unit test**, not a script, on the one
production parallel fixture the engine actually has, and compare the **complete** output
bit for bit at every worker count the roadmap names.

The harness is `tests/unit/engine/Poseidon/Core/test_worker_determinism.cpp` — 7 cases,
45 assertions, tags `[determinism]` / `[terrain]` / `[task_pool]`, in `PoseidonTests`.
It needs **no game data**.

## Why this fixture and not another

`Landscape::GenerateSegmentInto`, dispatched from `LandCache::Fill` pass 3
(`Landscape.cpp:1358`), is the **only** `TaskPool::ParallelFor` in the Poseidon library.
The audit behind that claim:

| Site | Where | In scope |
|---|---|---|
| `LandCache::Fill` pass 3 → `GenerateSegmentInto` | `World/Terrain/Landscape.cpp:1358` | **yes — the fixture** |
| Terrain plate build | `WgpuRenderer/TerrainWgpu.cpp:1262` | no — renderer, not linked into `PoseidonTests` |
| Terrain range build | `WgpuRenderer/TerrainWgpu.cpp:1762` | no — same |
| `WaveOAL::DecodePcm` | `PoseidonOpenAL/WaveOAL.cpp:692` (`Submit`) | no — audio, not authoritative, separate lib |
| Mission audio manifest preload | `World/WorldInit.cpp:760` (`Submit`) | no — fire-and-forget preload |
| `ObjectStreamPreparer` | `World/Terrain/ObjectStreamPrepare.cpp` | no — its own `std::thread` pool, not `TaskPool` |

`GenerateSegmentInto` was also the right pick for a second reason: its call site **already
has a serial arm**. `Fill` runs a plain `for` loop when the pool is null or there is a
single miss, so "serial task execution" is an existing production path here and did not
have to be invented for the test.

**There is no general serial mode, and none was added.** `TaskPool` has no bypass switch;
what exists is `TaskPool(1)`, which sets enkiTS's `numTaskThreadsToCreate = 0` so the
calling thread does all the work — that is serial *execution*, but still through the
scheduler and its partitioning. Both are exercised as separate arms, because they are
different things: arm 0 is the plain loop, arm 1 is the scheduler with one thread. Adding
a `TaskPool::SetSerial()` was considered and rejected: with exactly one consumer, which
already branches on a null pool, it would be a second untested code path standing in for
the one the game actually runs.

## What the harness does

One `Landscape` is built once and shared by every arm — 64x64 land over 128x128 terrain
(subdivision level 1, so the mesh is refined 2x per land cell and the inner subdivision
loops, shared-vertex candidate lists and seam quads are all live), a synthetic height field
with a real coastline, three texture slots of which two are **real `Texture` objects**
loaded from the JPEG fixtures, and per-cell `_random` UV offsets. Sharing one landscape is
deliberate: it removes "the arms were given different input" as an explanation for either
agreement or disagreement.

64 segments are generated per arm. Each is hashed **whole** (FNV-1a over: every vertex
position, normal, UV and clip flag as raw IEEE-754 **bits**; the shape's texture count;
every face's arity, vertex indices, special flags and texture *index*; the bounding box;
and the same again for the water sub-mesh). Bits, not values — `-0.0f == 0.0f` and
`NaN != NaN`, so a value comparison is simultaneously too loose and too tight to be the
strict comparison the gate asks for. Texture identity is hashed as an **index into the
shape's own table**, never as a pointer, both because an address is allocator state
(SIM-803 is an entire decision record about ranking by one) and because the index also
catches a divergence in registration *order* — which is what the shared
`GSegTextureRegMutex` guards.

Arms, all compared elementwise against serial:

| Arm | Threads | Result |
|---|---|---|
| serial (plain `for`, `pool == nullptr`) | — | reference |
| 1 worker | calling thread only | identical |
| 2 workers | 2 | identical |
| 4 workers | 4 | identical |
| typical desktop (`ResolveTaskPoolThreadCount(-1, hw)`) | 8 | identical |
| oversubscribed (`max(16, 2x cores)`) | 32 | identical |
| 4 workers x 3 repeats | 4 | identical |
| serial / 1 / 4 / typical with `CaptureMainFpEnvironment()` first | — | identical |

**No worker-count divergence was found.** That is the honest result and it is not a
surprise: each work item writes to its own pre-allocated `LandSegment`, reads a landscape
that nothing mutates, uses no RNG, and performs no cross-item reduction. The one shared
mutable thing in the loop — `Shape::RegisterTexture` under `GSegTextureRegMutex` — writes
into the *per-segment* texture table, so the lock protects the texture bank rather than
ordering the output.

### The two vacuous passes it is defended against

A determinism test that agrees because it generated nothing, or because everything hashed
to one value, passes for ever and means nothing. Both are pinned:

- **`Terrain fixture is non-degenerate`** measures the fixture: 64/64 segments have land
  faces, 64/64 have water faces, 64/64 have a bound texture, 33,506 faces and 36,381
  vertices in total.
- **The distinct-hash guard** requires more than 32 distinct values among the 64 segment
  hashes; it measures 64.
- **`Segment hash has teeth`** is the negative control: same rect twice must agree; the
  same rect at LOD 1 must *differ*; a different rect must *differ*.

## The finding: regenerating into a used segment appended a second copy of the mesh

The harness's negative control failed on its first run, and not on the axis it was built
for. `Landscape::GenerateSegmentInto` takes a caller-owned `LandSegment*` for exactly one
reason — so a segment can be **re**generated rather than reallocated; that is the entire
difference from `GenerateSegment`, which news one and returns it. Nothing in the tree
regenerates one, so the reset had never been exercised, and it was incomplete in two
places:

1. **The land branch cleared `_table`'s FACES and never its VERTICES.** `AddVertex`
   appends, and the per-call `candidates` lists start empty, so nothing was shared with
   the previous pass and a second generation appended a **complete second copy** of the
   mesh. The new faces index the new copy, so the segment still **draws correctly** —
   there is no visual tell at all. The symptom is a vertex buffer that grows without
   bound. Measured, same rect, four consecutive generations into one segment:

   | pass | `NVertex()` |
   |---|---:|
   | fresh | 571 |
   | 1 | 1,056 |
   | 2 | 1,541 |
   | 3 | 2,026 |

   `Compact()` does not save it: it shrinks capacity to size, it does not drop
   unreferenced vertices. The stale vertices also enter `CalculateMinMax`, so the bounding
   box becomes the union of every generation the segment has ever held.

2. **`_wTable` was only cleared on the branches that then refilled it.** A segment
   regenerated from wet to dry kept its previous sea mesh. This is the worse of the two:
   it would ship *stale terrain*, not merely waste memory.

**Both are latent at this commit.** Every caller today — `GenerateSegment`, and
`LandCache::Fill` pass 2 — hands over a freshly allocated segment, and `LandCache` slots
are replaced rather than recycled. So this is not a bug the owner can see today, and the
fix changes nothing about the shipping frame. It is filed and fixed because segment
reuse is the obvious next step for the LandCache and is exactly what this signature exists
to allow, and because a bug whose only symptom is unbounded VRAM growth with no visual tell
is not one anybody would find by looking.

**The fix** is two lines at the top of `GenerateSegmentInto` — `seg->_table.Clear();
seg->_wTable.Clear();` — with the reasoning in a comment beside them. The redundant
`Clear()` calls further down were left alone. Ablating the two lines makes
`Regenerating into a used segment leaves no residue` fail 7 of its 23 assertions, which is
how the numbers above were measured.

## What is NOT covered

- **No simulation, AI or gameplay state is exercised at all.** Terrain segment generation
  is *render* geometry. It is the only production `ParallelFor` that exists, so it is the
  only thing there is to validate — but the gate's language about "event order, selected
  targets, hits, deaths, ownership changes or replicated state" is about jobs Phase 8 has
  not written yet. **When simulation jobs land, this harness does not cover them**, and the
  reusable pieces to extend (the bit hasher, `RunAtWorkerCount`, `RequireIdentical`, the
  two anti-vacuity guards) are file-local by design so the next author copies them
  deliberately rather than inheriting an ill-fitting base class.
- **No record/replay, and no "first divergent authoritative state boundary" in the
  roadmap's full sense.** `RequireIdentical` reports the first divergent *segment index*,
  which is that idea at the scale this fixture has. There is no tick timeline here to
  bisect.
- **No TSan run.** The roadmap asks for TSan alongside this; the TSan preset is Linux-only
  (`linux-x64-clang-tsan`) and this session is on Windows. `TaskPool.cpp` already carries
  explicit `__tsan_acquire`/`__tsan_release` annotations for this exact fixture, so the
  ground work is done; the run is not.
- **No cross-platform comparison, and no tolerance policy** — deliberately. Everything here
  is same-build, same-platform, strict-equality. Nothing in this harness accepts a
  tolerance, and nothing should: the gate says tolerance is a diagnostic tool, and no
  diagnostic was needed.
- **The renderer's two `ParallelFor` sites are untested.** `TerrainWgpu.cpp:1262` and
  `:1762` are in `WgpuRenderer`, which `PoseidonTests` does not link. They build GPU plate
  and range data, not authoritative state, but they are parallel and unmeasured.
- **`ObjectStreamPreparer` is untested.** It runs on its own `std::thread` pool with a
  hard-coded worker count (2), not on `TaskPool`, and since 2026-08-30 it runs full
  `ShapeAdapter` geometry conversion whose result the main thread installs verbatim. That
  is a genuine parallel authoritative-ish path with no determinism coverage; it needs game
  data to drive, which is why it is not here.
- **The fixture is synthetic terrain, not a real `.wrp`.** The *kernel* under test is the
  unmodified production function, but its inputs are generated. A real world would widen
  the height/texture distribution; it would also make the test need game data and skip on
  this box, which is worse.
- **`GetTL()` is whatever `EngineDummy` returns.** The `centered` branch in
  `GenerateSegmentInto` (segment-local coordinate offsets) is therefore exercised in one
  configuration only.
- **The fix was not validated in the game.** Another agent may hold the single allowed
  instance and the owner is asleep, so verification here is build + unit tests. It is a
  no-op for every caller that exists, which is why that is acceptable; a reviewer who wants
  more should exercise a LandCache that recycles slots, which does not exist yet.

## Engine surface added

`LandSegment` gained five `const` accessors (`GetTable`, `GetWaterTable`, `SomeWater`,
`OnlyWater`, `GetLodLevel`). Its geometry was reachable only through `friend class
Landscape` / `friend class LandCache`, which is right for mutation but left a generated
segment unverifiable from outside the terrain tree — and the gate asks for the **full**
output, not a spot check. They add no state and do not widen the mutation surface.

## Evidence

Build: `win-x64-clang-rwdi`, worktree `.claude/worktrees/agent-det`, targets `PoseidonTests`
and `PoseidonGame`, clean.

| run | test cases | assertions | failures |
|---|---:|---:|---:|
| baseline (`safety/asset-obs-001`, `e1b1e354`) | 2968 | 235,135 | 0 |
| after this change | 2975 | 235,180 | 0 |

Difference is exactly the 7 new cases / 45 new assertions. 20 cases skip in both runs
(external game data absent on this box).
