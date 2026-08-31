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

## What this changes about the plan

Step 5 as written — *"put the renderer on its own thread"* — is shape 3's problem wearing
a different hat, and it cannot be the next commit. The order that follows from the counts
above is: settings into the frame, then the instance queue, then the upload path, and only
then a thread. Each of the first three is independently testable **with everything still
serial**, which is what the phase gate wants anyway.

## What this does not say

- **No frequencies were measured.** The table says which call sites exist, not how often
  each fires in a steady frame. Shape 3's cost in particular depends on whether streaming
  creates meshes every frame or only on level change, and that is unmeasured.
- **It assumes one renderer instance.** True today; a second would make the
  "exactly one owning thread" rule a per-instance rule rather than a global one.
- **It does not touch GL33.** GL33 keeps compiling and gets no new architecture, per the
  standing constraint.
