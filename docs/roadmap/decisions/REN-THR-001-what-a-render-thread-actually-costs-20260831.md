# REN-THR-001 — What stands between here and a render thread, counted

**Date:** 2026-08-31
**Branch:** `asset-obs-001`
**Roadmap:** Phase 5, steps 4–5 — *"build the producer/consumer handoff"*, *"put the renderer on its own thread"*

Phase 5's buffer blocker is closed (`84a44e79`): the frame's slices now point into a
two-slot ring rather than at members the next frame overwrites. That was the blocker
everyone was looking at. This document is about the one underneath it, which is larger,
and which decides what step 5 actually is.

**No code changed here.** This is the audit the plan asks for before implementation.

## The blocker: the handle is exclusive, and 62 of 79 entry points say so

Every FFI entry point in `engine/WgpuRenderer/rust/src/ffi.rs` takes `*mut WgrRenderer`
and immediately forms `&mut *renderer` — an **exclusive** Rust reference:

| | count |
|---|---:|
| `wgr_*` entry points | 79 |
| forming `&mut *renderer` | **62** |
| forming a shared `&*renderer` | 9 |

Two threads inside any two of those 62 at once is aliasing UB, and it is UB *in Rust's
model* — it does not matter that `wgpu::Device` and `wgpu::Queue` are themselves `Send +
Sync`, and it will not show up as a wgpu validation error. So "put `wgr_render_frame` on
a thread" is not a thing that can be done on its own. **Exactly one thread may hold the
handle**, and the question is which calls have to move to it.

## Which calls are actually in the way

Not all 62. What matters is the ones the producer makes *while a frame is rendering* —
that is, outside `NextFrame`. Grouped by the C++ function that makes them
(`EngineWgpu.cpp`):

**In the way — producer-side mutation of renderer state:**

| Caller | Calls | Driven by |
|---|---|---|
| `SceneObjectCreated` / `SceneObjectMoved` / `SceneObjectRemoved` | `wgr_instance_add` / `_update` / `_remove` | **the simulation** |
| `EmitGpuProxies` / `RemoveGpuProxies` / `SetFarProxyInstances` | `wgr_instance_add` / `_remove` / `wgr_far_set_instances` | traversal |
| `RegisterGpuModel` / `CreateVertexBuffer` / `DrawSectionTL` / `UntexturedAlbedoHandle` | `wgr_mesh_create` / `_destroy`, `wgr_model_register`, `wgr_register_crown_centres` | asset streaming |
| `OverlayTextureCreate` / `Update` / `Destroy` | `wgr_texture_*` | UI |
| `PushRenderParams`, `PushSceneCamera`, `PushSkyRuntime`, `SetGrassSettings`, `SetCullDebugSettings`, `SetTemporalTuning`, `SuppressWorldObjects` | 12 `wgr_set_*` | per-frame settings |

**Not in the way:** the stats readers (`GetObjectStats`, `GetGpuTimings`,
`GetGrassStats`, `GetMemoryStats`, `GetTemporalInfo`, …) are read by the dev panel after
the frame, and `ResizeSurface` / `SetSwapInterval` / `Screenshot` are user events that can
be queued like anything else.

So the list is about **ten call sites**, not sixty-two. That is the useful number, and it
is why this is a tractable piece of work rather than a rewrite.

## The three shapes, and what each costs

1. **The `wgr_set_*` settings.** Pure value pushes with no return. Cheapest to move:
   they become fields the producer fills and the consumer applies at the top of the
   frame. Costs a `WgrFrame` extension and an ABI version bump — which the existing
   handshake already exists to catch.
2. **Instance add/update/remove.** These have to become queued commands, because the
   producer issues them from simulation callbacks at arbitrary points in the tick.
   Note the convergence with SIM-812: `SceneObjectMoved` is on that audit's short list of
   *output-only* writes — the one class it names as genuinely incremental. **The same
   call is the first slice from both sides**, which is the strongest argument for
   starting there.
3. **Mesh/texture/model creation.** Hardest, because the producer wants a **handle back
   immediately** — `wgr_mesh_create` returns an id the caller stores. A queued command
   cannot return synchronously, so this needs either an id allocated producer-side and
   fulfilled later, or a second, genuinely `Send` upload path that does not go through
   the exclusive handle. This one is a design decision, not a refactor.

## Shape 3, resolved: the synchronous handle is not load-bearing

Shape 3 was left above as "a design decision, not a refactor", because
`wgr_mesh_create` returns an id the caller checks immediately —
`if (!mesh) return nullptr;`, choosing the non-retained path on failure. A queued
command cannot answer synchronously, so that fallback looked like it forced the whole
upload path to stay on the producer's thread.

It does not, and the reason is one line in `gfx3d/pool.rs:468`:

```rust
if verts.is_empty() || indices.is_empty() {
    return None;
}
```

That is the **only** failure path in `Pool::alloc`. Everything after it grows the pool and
succeeds; there is no exhaustion case and no fallible device call. So the condition the
C++ fallback is testing for is *empty input* — which the caller can test itself, before
calling, without the pool and without the renderer. **The id can therefore be allocated
producer-side and the create queued**, and nothing about failure handling is lost. The
empty-input guard moves from "check the returned handle" to "do not enqueue an empty
mesh", which is a better place for it anyway.

`mesh_update` is easier still: `Pool::update_verts` already takes `&self` and writes
through `&wgpu::Queue`, both of which are `Sync`. It is only the `&mut *renderer` at the
FFI boundary that makes it exclusive.

### But uploads still cannot run *during* a frame

Resolving the handle question does not make creation concurrent with drawing, and this is
the real constraint on shape 3. `Gfx3d::mesh_create` clears `bake_bind_cache` and
`bake_bind_cache_prev` whenever the pool's generation changes (`gfx3d/mod.rs:4576`),
because pool growth reallocates the vertex buffer that every cached bind group references.
The draw path **reads that cache** — `bake_bind_cache.get(&g.mesh)` at `mod.rs:4923`, with
a per-frame swap at `4738`. A create landing mid-frame therefore destroys bind groups the
frame in progress is still using. That is a genuine data dependency, not an aliasing
technicality, and no amount of `Send` fixes it.

So creates must be **drained at a frame boundary**, not merely moved to another thread.
Which is the same mechanism shape 2 needs, and it collapses the three shapes into one:
a queue the producer fills and the consumer drains at one defined point per frame. That
is a considerably better answer than three separate mechanisms, and it is what the next
implementation step should build.

## Shape 2, examined: the instance table is not a renderer resource at all

`wgr_instance_add` returns a slot the caller stores and later passes to update and remove,
so on the face of it it has shape 3's problem too. It does not, and for a different
reason: follow it down and there is **no device in it**.

`Gfx3d::instance_add` → `Cull::instance_add` → `InstanceTable::add` (`cull.rs:404`) is a
free list plus a generation counter. Pop a free slot or push a new one, bump the
generation, mark it dirty, return `handle(slot, generation)`. No `Device`, no `Queue`, no
allocation that can fail, and the result is a pure function of the sequence of adds and
removes. The GPU only sees this table later, when the dirty range is uploaded during the
frame.

That makes shape 2 a different question from the one this document started with. Queuing
these as commands would work, but it treats as a renderer resource something that is
merely bookkeeping the renderer happens to host. The alternative — the producer owns the
table and publishes it with the frame, exactly as the ring now publishes the draw arrays —
is a better fit for where the thread boundary is being drawn, and it removes three of the
ten call sites rather than relaying them.

It is also the riskier of the two, and the risk has a name. `stale_instance_ops`
(`cull.rs:993`) counts handles used after their instance was removed, and its comment says
it exists because this exact identity bookkeeping, done C++-side, *"used to corrupt a live
instance silently"*. Moving the table back across the boundary re-opens the ground that
counter was put down to hold. **Any such move has to keep that counter and prove it stays
zero**, which is a gate the change can be held to rather than a reason not to try.

Not decided here. The measurement that decides it is how many add/update/remove operations
a steady frame actually issues — unmeasured, and named again below.

## The frequencies, measured — and the world you measure on decides the answer

This document twice said the per-frame frequency was unmeasured and twice said it was the
number that decides the design. It is measured now, by counters at the call sites reported
with their denominator in frames (`Renderer mutation ops` in the graphics log).

**Abel, the standard bench scene, 98,684 registered instances:**

| | steady state | at load (peak frame) |
|---|---:|---:|
| instance add | **0.00 / frame** | 12,627 |
| instance update | **0.00** | 97,260 |
| instance remove | **0.00** | 849 |
| mesh create | **0.00** | 704 |

Zero, over windows of 140–159 frames, repeatedly — and zero again with a **moving** camera
(the vehicle-velocity mission), which was the obvious objection. `registered instances`
never budges from 98,684 for the whole run. Abel registers the entire island at load and
then streams nothing at all.

**Everon (the 103 MB substituted world), 112,613 registered, 15,108 drawn, camera standing
still at the README's pose:**

| | mean / frame | peak frame |
|---|---:|---:|
| instance add | **94 – 513** | 1,167 |
| mesh create | **2.8 – 15.5** | 80 |
| model register | 0.25 – 1.7 | 10 |
| instance update / remove | 0.00 | 0 |

Sustained, ten windows in a row, **while standing still**. So the honest answer to "how
much traffic does the queue carry" is: *it depends entirely on whether the world fits in
the retained set*, and the bench scene is the case where it does. Measuring only on Abel
would have concluded the queue was free and sized it accordingly.

Three things follow.

1. **The queue is a hot path.** Five hundred appends in a frame is fine for a vector of
   PODs and is not fine for anything that takes a lock per operation. That rules out the
   cheap implementation the Abel numbers alone would have justified.
2. **Creates cannot be deferred far.** 8–15 mesh creates per frame, sustained, is the
   world filling in as you look at it. A drain that runs once per frame is fine; one that
   batches across frames to smooth cost would show up as geometry arriving late.
3. **The instance table does not churn.** Updates and removes are flat zero in the steady
   state on both worlds — the stream only ever admits. That makes the "producer owns the
   table and publishes it" option from the previous section considerably more attractive
   than it looked, because there is no reconciliation of concurrent edits to get wrong.

Caveats, since one number here is load-bearing for a design: **one run per world**; the
Everon run's frame had 30.1% unattributed residual, so it is a streaming frame and not a
settled one; and `WGR_OBJECT_STREAM_MAX_OBJECTS=200000` means the cap was never reached,
which is why removes stayed at zero — a run that hits the cap would evict, and that case
is unmeasured.

## What this changes about the plan

Step 5 as written — *"put the renderer on its own thread"* — is shape 3's problem wearing
a different hat, and it cannot be the next commit.

With shape 3 resolved the three shapes are **one mechanism**, not three: a command queue
the producer fills and the consumer drains at one defined point per frame. Settings are
the trivial case of it, instances the incremental one, creates the one that made the queue
necessary. Building one queue and moving the ten call sites onto it is a smaller job than
the three-part sequence this document originally proposed, and every step of it is
testable **while everything is still serial** — which is what the phase gate wants anyway.

Only after all ten sites go through the queue does "one thread owns the handle" become a
statement that can be true.

## What this does not say

- **No frequencies were measured.** The table says which call sites exist, not how often
  each fires in a steady frame. Shape 3's cost in particular depends on whether streaming
  creates meshes every frame or only on level change, and that is unmeasured.
- **It assumes one renderer instance.** True today; a second would make the
  "exactly one owning thread" rule a per-instance rule rather than a global one.
- **It does not touch GL33.** GL33 keeps compiling and gets no new architecture, per the
  standing constraint.
