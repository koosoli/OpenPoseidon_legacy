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

---

## Addendum — the oracle used for its actual purpose: build against build

Everything above was verified the only way available at the time: identical counts, identical
image, zero stale-handle refusals, per slice, serially. That is a good gate and it is not the
gate Phase 5 step 6 asks for, which is *this restructuring published the same frames*.

With SIM-816's determinism mode and the handle-masking hash, that comparison is now possible
**between two different binaries**, and the slice that could least be verified headlessly —
the overlay textures (REN-THR-010), whose translation path never fires without an open
dev-panel window — was measured retroactively.

Two builds differing only in `EngineWgpu.{cpp,hpp}` (`4d59e813` versus `075f3dba`), each run
once under `POSEIDON_LOCKSTEP_HZ=60 WGR_LOD_GOVERNOR_RANGE=1 WGR_FRAME_HASH_MASK_IDS=1`,
frames aligned by simulation tick:

| | |
|---|---:|
| tick-aligned frames compared | 901 |
| **content-identical across all seventeen slices** | **900 (99.9%)** |
| differing | `verts`, in one frame |

That is the same figure as two runs of the *same* build (SIM-816), and the same single `verts`
frame — so the overlay slice contributed **no** observable difference, and the one outlier is
the pre-existing run-to-run blip rather than anything this change did. A cross-build
comparison that lands exactly on the run-to-run noise floor is the strongest statement this
oracle can currently make.

**A trap worth recording, because it nearly invalidated the measurement.** Arm A was built by
checking out the two files at the older commit while `HEAD` stayed where it was — and
`Deploy.ps1` stamps `DEPLOYED-FROM.txt` from **HEAD**, not from what was compiled. The stamp
read `075f3dba` while the binary was the `4d59e813` renderer. The stamp is a guard against
deploying a stale *tree*; it cannot see a patched working copy, and rule 0's "suspect the
binary" advice depends on the stamp being true. For an A/B built this way the bookkeeping has
to be the operator's, and the arm names have to carry it.
