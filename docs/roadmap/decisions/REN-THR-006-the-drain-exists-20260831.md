# REN-THR-006/007/008 — The producer/consumer drain exists, in three verified slices

**Date:** 2026-08-31
**Branch:** `asset-obs-001`
**Roadmap:** Phase 5, step 4 — the render-thread handoff. Design: REN-THR-001.

REN-THR-001 counted 62 of 79 FFI entry points forming `&mut *renderer` and concluded the
three shapes of producer-side mutation collapse into **one queue drained at one point per
frame**. That queue now exists. Everything below is live in `EngineWgpu` and drained at the
top of `NextFrame`, in this order: settings, resources, instances.

## Slice 1 — settings (`cc20b525`)

Seven latches carry every `wgr_set_*` the producer fired mid-frame: suppress-world (from
`World::Simulate`, mid-tick), cull debug, temporal tuning, grass params + card spacing,
object-cull params, far-tier params. The setter computes the exact FFI payload at call time
— all clamps and env gates still read producer state freely — and the drain pushes dirty
ones. Last-write-wins is what back-to-back direct calls already meant.

## Slice 2 — instances (`ed58c9af`)

The producer no longer holds a renderer slot anywhere. `_gpuInstances` and `_gpuProxies`
hold producer handles; add/update/remove are FIFO ops translated at the drain. FIFO is
load-bearing: a remove frees its handle at enqueue, a same-frame add may reuse it, and the
drain still executes the remove against the old slot first. The renderer's slot generations
(ID-3) remain underneath as the tripwire — a mistranslation surfaces as the stale-instance
ANOMALY line.

## Slice 3 — retained meshes and models (this commit)

Mesh creates move their vertex/index vectors into the op (zero copy), destroys carry the
producer handle, and model registration ships its lods/sections/materials with **producer
mesh handles in the sections**, translated at the drain. Two facts made this possible
(REN-THR-001): `Pool::alloc`'s only failure is empty input, now checked producer-side; and
the crown-centre **base**, which is baked into vertex data, is applied at the drain where
the real base exists, via per-op `crownComp` offsets. Instance payloads' `model` field is
translated the same way; an untranslatable handle is a loud dropped op, never a
pass-through, because a passed-through wrong id is exactly the walls-gone failure of
`bc39225d`.

## Verification, each slice

Same discipline each time: full suite (389,910 assertions, 0 failures), then a 2,500-frame
Everon combat run at the owner's pose — the scene with measured queue traffic, not the bench
scene REN-THR-001 showed carries none. Slice 3's run: peaks of 4,110 instance adds, 46,234
updates, 795 removes, **734 mesh creates and 14 model registers in single frames**, zero
ANOMALY lines, identical registered count (52,507) and identical scene image throughout.

## What still touches the handle outside the drain, deliberately

| What | Why it stays, for now |
|---|---|
| `VertexBufferWgpu` (create/update/set-skin/destroy) | CPU-path meshes: handles ride inside `WgrDraw3D` payloads and per-frame `wgr_mesh_update` is *upload traffic*. The thread answer is a genuinely `Send` upload path (`Pool::update_verts` is `&self` + `Queue`), not a queue that copies vertex arrays every frame. |
| Overlay textures (`wgr_texture_*`) | UI; same synchronous-handle story, low traffic, next in line. |
| `SetFarProxyInstances` | Bulk upload at world load (~117 MB source rows on Chernarus-size worlds). |
| `SetSwapInterval`, `ResizeSurface`, screenshots | Synchronous answers / surface ownership — stop-the-world class, like device recreate. |
| Stats readers (`GetObjectStats` etc.), `HasGrassPhotoClumps`, `SetGrassSettings`'s placement witness | **Reads.** The snapshot problem; owned by the thread slice. |

## What this does not prove

Nothing here proves thread-safety — everything is still serial, and that is the point: each
slice was provable serially (identical counts, identical image, zero stale-handle refusals)
before any concurrency exists to blame. The drain-to-`wgr_render_frame` span is now the
window a render thread would own; making that literal requires the table above to empty and
the equivalence oracle's remaining simulation divergence (REN-THR-005, SIM-815 in flight) to
be closed or bounded.
