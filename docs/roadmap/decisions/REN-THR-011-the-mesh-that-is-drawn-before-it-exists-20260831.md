# REN-THR-011 — The mesh that is drawn before it exists

**Date:** 2026-08-31
**Branch:** `agent-cpumesh` (from `asset-obs-001`)
**Commit:** `0a3992ba`
**Roadmap:** Phase 5, step 4 — the render-thread handoff. Design: REN-THR-001. Drain:
REN-THR-006/007/008/010. Uploader: REN-THR-009.

REN-THR-006's table of what deliberately still bypasses the drain had the CPU-path mesh
pipeline at the top of it. REN-THR-009 took the per-frame half off the exclusive handle —
`wgr_mesh_update` and the deferred destroy now go through a `Send + Sync` uploader that never
forms `&mut *renderer` — and named what was left: `wgr_mesh_create` in
`EngineWgpu::CreateVertexBuffer`, and `wgr_mesh_update` + `wgr_mesh_set_skin` in
`VertexBufferWgpu::SetSkinData`. Both are structural, both are creation-time, and both are
now `ResourceOp`s drained at the top of `NextFrame` in the same queue and at the same point
as the retained path's.

That part is a refactor. The part worth writing down is where a producer handle becomes a
renderer handle, because the obvious answer is wrong for a reason the code says out loud.

## The decision: translate at the publish point

Two candidates, from the brief.

**(A) Hand the buffer its renderer handle back at the drain.** Attractive: no per-draw work,
no translation table, the buffer holds one number. **Rejected, and not on cost — it cannot
answer the question in time.**

`SceneShadowPass.cpp`:

```cpp
572    if (!s->GetVertexBuffer())
573    {
574        s->ConvertToVBuffer(s->NVertex() > 1024 ? VBBigDiscardable : VBSmallDiscardable);
...
600    GEngine->AddShadowCaster(*s, obj->Transform());
```

Twenty-six lines apart. The buffer is created and the caster naming it is recorded inside the
same draw phase, and the drain that would define the mesh runs at the top of the `NextFrame`
that comes *after* that draw phase. At the moment `run.mesh` is written there is no renderer
handle to hand back, and there cannot be — the whole point of the drain is that the create
does not land until the frame boundary. Option A does not defer the problem, it deletes the
frame: every newly visible shadowed object would cast nothing until the frame after next.

This is why the brief's question *"does that happen? establish it, do not assume"* is the
hinge of the slice. It happens, it is the ordinary path for every newly visible shadowed
object, and it is not rare enough to special-case.

**(B) Translate `_draws3d[i].mesh` (and `_shadowCasters[i].mesh`) at the publish point.**
Adopted, in a form that costs nothing on the draws that do not need it:

- `DrawSectionTL` and `AddShadowCaster` write `buf->RendererMesh()` — the real handle for
  every mesh created in an **earlier** frame, which is the overwhelming majority, and free.
- A record that comes out `0` is remembered by **index** in a small side vector, together
  with a `shared_ptr` to the mesh's slot.
- `PatchPendingMeshHandles()` runs in `NextFrame` after every drain and **before** the arrays
  are swapped into the publish ring.

There is no map lookup per draw and no walk of the draw arrays. On a frame that created no
CPU mesh the whole mechanism is two `empty()` checks.

**Before the hash, not after, and that is load-bearing.** `HashPublishedFrame` masks
`draws3d`'s `mesh` field under `WGR_FRAME_HASH_MASK_IDS` (SIM-816) — but `shadow_casters`,
slice `sl[7]`, is **not** masked. Publishing a producer-side placeholder in a caster would
have moved that slice's hash on every frame with a newly visible caster, and the cross-build
comparison in REN-THR-006's addendum — the actual Phase 5 step 6 gate — would have started
reporting divergence that had nothing to do with the frame's content. What is published, and
what is hashed, is byte-for-byte what a direct `wgr_mesh_create` would have left there.

## Identity is a shared slot, not a handle plus a map

`_meshIdOf` translates the retained path's producer handles. The CPU path does not use it,
and the reason is the destructor.

A `VertexBufferWgpu` is destroyed wherever the last reference to its `Shape` drops. That can
be before the drain that would have defined its mesh. A `producer -> renderer` map entry
would then need reaping by code that cannot reach the engine, and a raw back-pointer from the
queue into the buffer would dangle at exactly that moment — the trap the brief names.

So identity is a `std::shared_ptr<CpuMeshSlot>`, held by the buffer, by the queued create, and
by any pending draw reference:

```cpp
struct CpuMeshSlot
{
    uint64_t producer = 0;  // from _nextMeshHandle -- ONE handle space with the retained meshes
    uint64_t renderer = 0;  // written at the drain; 0 = "the create has not landed yet"
    bool dead = false;      // the buffer died; a create still queued must be skipped
    std::vector<SVertex> verts;
    std::vector<VertexIndex> indices;
};
```

Shared ownership makes the slot outlive whichever of the two dies first, and turns *"the
buffer is gone"* into a flag the drain reads rather than a lifetime question nobody can
answer. There is nothing to reap and no map to keep in step.

The producer handle is still allocated from `_nextMeshHandle`, as the brief asks, so the two
mesh paths share one handle space and a CPU handle can never be confused with a retained one.
It is not what the *translation* keys on — that is the slot — but it is what the diagnostics
name, and it forecloses the aliasing question rather than arguing it away.

`SetSkinData` needs to append to the engine's queue and the buffer cannot own an
`EngineWgpu*`: it has no defined destruction order against the engine. `CpuMeshLink` is that
pointer with a kill switch — one shared object, set in the constructor, nulled first thing in
`~EngineWgpu`. A late `SetSkinData` reads the null and returns without marking the mesh
skinned, so a later call can retry rather than leaving a mesh permanently flagged as skinned
with no weights on the GPU.

## The three cases, and what each does now

### 1. A buffer created and destroyed before the drain ever runs

Happens constantly — `VBSmallDiscardable` shadow meshes are built and released inside a
single draw phase.

The destructor sets `slot->dead` **first and unconditionally**, then branches. With
`slot->renderer == 0` there is nothing to destroy, so it releases the staged vertex and index
arrays and returns. The drain sees `dead` on the `CpuMeshCreate` op and **skips the create
entirely**: no pool range allocated, no pool generation bump, no `bake_bind_cache` clear, for
a mesh nobody can name any more. Cancelling is strictly cheaper than fulfilling and then
destroying, and it is the only arm that does not briefly hand a live pool range to a dead
mesh. A `MeshSetSkin` op queued against the same slot finds `renderer == 0` and skips too.

With `slot->renderer != 0` the create already landed and the destructor is unchanged from
REN-THR-009: `wgr_uploader_mesh_destroy`, drained at the top of `wgr_render_frame`.

### 2. A draw submitted in the same frame the mesh was created

**Established, not assumed, and it is the normal case** — `SceneShadowPass.cpp:572-600`,
quoted above. `Shape::ConvertToVBuffer` is also reached from `ShapeBank`'s optimize pass
during streaming, so the colour path sees it too.

The ordering makes it work without any special handling. `InitDraw` clears the draw arrays;
the draw phase fills them; `NextFrame` **drains first**, then patches, then publishes, then
renders. So a create enqueued anywhere in the draw phase is defined before the frame that
references it is described. The draw record carries `0` at record time and the real handle by
the time anything reads it.

The index is safe because `_draws3d` and `_shadowCasters` only ever `push_back` between
`InitDraw` and the publish — nothing reorders or erases. The two pending vectors are cleared
at every site that clears the array they index, including the `WGR_SHADOW_FORCE_NO_CPU_CASTERS`
diagnostic that empties `_shadowCasters` mid-`NextFrame`, and the patch is bounds-checked
anyway.

The one case that resolves to `0` at patch time is case 1 crossing case 2: a draw recorded,
then the buffer released, both inside one draw phase. The draw is dropped and counted, with an
`INFO` line naming it. **That is not a behaviour change.** Today the record carries a handle
whose destroy is drained at the *top* of `wgr_render_frame`, before any draw is read, so the
renderer's `meshes.get` misses and the draw is dropped there instead. Same outcome, one fewer
freed-handle round trip, and now it says so.

### 3. The uploader's `update` and `destroy`, which take renderer handles

They still do. The buffer reads `slot->renderer` for both, so nothing about the uploader's
ABI, its metadata table, or REN-THR-009's lifetime contract changes — and in particular the
destructor still does not need to reach the engine.

What changes is `Update` when `slot->renderer` is still `0`: there is no mesh to upload into,
so it **rewrites what the queued create will upload** instead:

```cpp
if (slot->renderer == 0)
{
    slot->verts = std::move(verts);
    return;
}
```

The end state is byte-identical to today's create-then-update pair, one drain later. This is
not a nicety — it is what keeps the conformed-road case exact. `ObjectClasses.cpp`'s
`gpuConform` block depends on a road's **first** `Update` running inside a conform-plane draw
and replacing the `BuildVertices` bytes the create wrote with `BuildOrigVertices` bytes
carrying the ClipLand selector, in the same draw phase as the create. Skipping that upload
would have left the road rigid at its authored height for its first drawn frame; issuing it
against a mesh that does not exist would have dropped it entirely.

## `CreateVertexBuffer` is now infallible

REN-THR-001 established that `Pool::alloc`'s only failure is empty input (`pool.rs:468`);
everything after it grows the pool and succeeds, with no exhaustion case and no fallible
device call. The retained path at `RegisterGpuModel` already checks emptiness producer-side
for exactly this reason, with the comment saying so. The same shape is used here: `nv <= 0`
and `ni <= 0` are already rejected above, so the queued create cannot fail and there is no
returned id left to test.

That matters more here than it did on the retained path. `ShapeDraw.cpp:508` names
`_buffer` being null — *"`CreateVertexBuffer` returning null"* — as **the only structural
place the two backends can diverge** for a road. Making the create infallible removes that
divergence rather than deferring it.

## What was removed rather than kept as a fallback

`wgr_mesh_create` in `CreateVertexBuffer` and `wgr_mesh_set_skin` + its paired
`wgr_mesh_update` in `SetSkinData` are gone, not guarded. There is no path where the drain
cannot run and a buffer can exist: `CreateVertexBuffer` returns `nullptr` when `_renderer` is
null, so a buffer only exists when a renderer did, and `DrainResourceOps` runs from
`NextFrame` unconditionally. One way through.

The pre-existing `!uploader` fallbacks in `Update` and the destructor are untouched — they are
REN-THR-009's, they guard a different null, and this slice has no opinion on them.

## Verification

Built in this worktree (`.claude/worktrees/agent-cpumesh`), preset `win-x64-clang-rwdi`.
**Nothing was deployed and the game was never launched** — another session owns the game
folder and will run the gate below.

| Suite | Before (this worktree, `HEAD~1` files) | After |
|---|---|---|
| `PoseidonTests` | 3,111 cases / 389,942 assertions / **0 failures** | 3,111 / 389,942 / **0** |
| `PoseidonCoreTests` | — | 838 cases / 9,808 assertions / **0 failures** |
| `PoseidonFoundationTests` | — | 491 cases / **0 failures** (assertion total varies by design) |
| `wgpu_renderer` (Rust, rwdi) | 221 passed / 1 ignored / 0 failed | **221 passed** / 1 ignored / 0 failed |
| `OpenPoseidon.exe` | links | links |

The "before" column was **measured**, not quoted: the two changed files were restored from
`HEAD~1` in the working tree (`git restore --source`, never `git stash` — the shared stash
currently carries another agent's `agent-heap-race: wip2` entry), `PoseidonTests` rebuilt and
rerun, then restored. 389,942 either side. The brief quotes 389,947; that difference is
between checkouts, as REN-THR-009 already recorded, and is not something this commit did.

No Rust changed, so the Rust suite is a regression check rather than new coverage. Note that
`cargo test` run from the **main checkout** tests the main checkout's source, not this
worktree's — a run there came back 176 passed / 4 failed against somebody else's tree. Every
number above is from `cd` into the worktree first.

## The gate the other session should run

**A building proves nothing here.** Static world geometry goes to the retained GPU-driven
path (`RegisterGpuModel`), which this commit does not touch. CPU-path meshes are the
*animated and skinned* geometry: infantry, vehicles with moving parts, animated props, rotor
blades, terrain-conformed roads, and the CPU shadow casters for all of them. An intact town
under an unchanged sky is consistent with every one of those being broken.

**Scene:** an Everon combat mission at the owner's pose — the scene REN-THR-006/008 used for
their 2,500-frame runs, chosen because it is the one with measured queue traffic (Abel
registers the whole island at load and streams nothing). Sun low, so shadows are long.

Five things to look at, in the order they would fail:

1. **Infantry animate.** Watch moving soldiers for several seconds. If the `MeshSetSkin` op
   never drains, `skinned` is already true producer-side so a bone palette is still attached,
   but the GPU has no skin buffer — the failure is a character frozen at bind pose or
   collapsed to a point, not a missing one. This is the single most likely thing to be wrong
   and the cheapest to see.
2. **Newly visible animated objects appear on the frame they become visible.** Turn the camera
   onto units that were not drawn a moment ago, or watch units come over a rise. A one-frame
   hole, or a unit that pops in late, is the patch path failing — case 2 above.
3. **Their shadows appear at the same time, and posed.** This is the call site that decided
   the design. A soldier drawn but shadowless, or a shadow that lags the soldier by a frame,
   is `_castersPendingMesh` not doing its job. A soldier whose shadow is in a *different pose*
   is the skin op landing after the caster rather than before.
4. **Roads conform to sloped ground.** Look along a road across a hill. Rigid segments
   floating above or buried in the terrain mean the pre-drain `Update` rewrite lost the
   ClipLand selector — case 3 above.
5. **The log line.** `"Wgpu CPU mesh: N draw records named a mesh whose create was cancelled"`
   is expected at scene churn in small numbers. A steady stream every frame would mean the
   cancel is firing on live meshes, and that is a bug in case 1 rather than a diagnostic.

**And then the real gate, which is stronger than any of the five:** the cross-build
equivalence oracle from REN-THR-006's addendum. This commit versus its parent, each run once
under `POSEIDON_LOCKSTEP_HZ=60 WGR_LOD_GOVERNOR_RANGE=1 WGR_FRAME_HASH_MASK_IDS=1`, frames
aligned by simulation tick, **excluding frames whose `draws3d` is empty** (the loading screen
and the teardown frame — that exclusion is a rule of the method now). All seventeen slices
should be content-identical. Slice `sl[7]`, `shadow_casters`, is the one to read first: it is
the only slice carrying an unmasked mesh handle, so it is the slice a mistranslated caster
would move, and it is exactly the case this change is most likely to get wrong.

Arm-naming trap, repeated from REN-THR-006 because it nearly invalidated that measurement:
`Deploy.ps1` stamps `DEPLOYED-FROM.txt` from **HEAD**, not from what was compiled. An A/B
built by checking out files at another commit produces a stamp that lies. Carry the arm names
in the operator's own bookkeeping.

## What this does NOT prove

- **Nothing here proves thread-safety.** No second thread exists. What is proved is that the
  CPU mesh path's two structural calls no longer touch the exclusive handle outside the drain,
  and that the serial behaviour is unchanged as far as the suites can see.
- **No in-game verification at all.** Another session owns the game folder; nothing was
  deployed and nothing was launched. Everything above is the suites, the link, and reading.
  The five checks are a gate that has not been run, not evidence.
- **The suites do not exercise this path.** There is no unit test for `VertexBufferWgpu` —
  it lives in an anonymous namespace and needs a live renderer. `389,942 = 389,942` means
  nothing this change did was visible to the existing tests, which is a much weaker statement
  than "it works". The Rust side gained no test because no Rust changed.
- **Case 1 crossing case 2 is argued, not measured.** The claim that a draw naming a
  cancelled create is dropped identically today rests on reading `wgr_render_frame`'s
  destroy-drain ordering, not on a capture of it happening.
- **Pool residency may differ marginally.** REN-THR-009 already deferred destroys to the next
  frame's start; this defers creates too. A frame that creates and destroys CPU meshes heavily
  now does both one boundary later. Unmeasured in a real scene, and invisible to the suites.
- **It does not empty REN-THR-006's table.** `SetFarProxyInstances`, `ResizeSurface`,
  `SetSwapInterval`, screenshots and every stats reader still form `&mut *renderer`. What it
  does do is close the row that table listed first.
- **It does not touch GL33.** Standing constraint; `EngineGL33::CreateVertexBuffer` is
  unchanged.

## What is left before a render thread can start

Unchanged from REN-THR-009's list except that its item 1 is now done:

1. ~~`wgr_mesh_create` and `wgr_mesh_set_skin`~~ — this document.
2. **`SetFarProxyInstances`** — bulk, load-time, ~117 MB of source rows on Chernarus-size
   worlds. Needs a decision about ownership, not a queue.
3. **The reads.** Every stats reader, `HasGrassPhotoClumps`, the grass placement witness.
   The snapshot problem; belongs to the thread slice itself.
4. **`ResizeSurface` / `SetSwapInterval` / screenshots** — stop-the-world class.
5. **The simulation divergence** REN-THR-005 records (SIM-815 in flight) must be closed or
   bounded, because the equivalence oracle is what will decide whether a threaded frame is the
   same frame.
