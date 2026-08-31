# REN-THR-009 — The upload path that does not need the handle

**Date:** 2026-08-31
**Branch:** `agent-upload2` (from `asset-obs-001`)
**Roadmap:** Phase 5, step 4 — the render-thread handoff. Design: REN-THR-001. Drain: REN-THR-006/007/008.

Four slices of the producer/consumer drain are done — settings, retained instances,
retained meshes/models, overlay textures. REN-THR-006's table of what deliberately still
bypasses the drain has the CPU-path mesh pipeline at the top of it, with the reason
written out: per-frame `wgr_mesh_update` is *upload traffic*, and queuing it would copy a
full vertex array every frame for every skinned character. This document is the audit of
that path and the surface built on the audit's answer.

## Part 1 — the audit

Four entry points, followed to the pool with file:line. The question asked of each: does it
touch anything a concurrent render pass reads **structurally** — bind groups, allocation
tables, generation counters — or only buffer **contents** through `Queue::write_buffer`?

### `wgr_mesh_update` — contents only

- `ffi.rs:2087` forms `&mut *renderer`, calls `Renderer::mesh_update`.
- `lib.rs:5477` → `self.gfx3d.mesh_update(&self.queue, handle, verts)`.
- `gfx3d/mod.rs:4651` takes `&mut self` **and never uses it mutably**:

```rust
pub fn mesh_update(&mut self, queue: &wgpu::Queue, handle: u64, verts: &[WgrMeshVertex]) {
    let Some(mesh) = self.meshes.get(KeyData::from_ffi(handle).into()) else { return; };   // shared get
    if verts.is_empty() || verts.len() as u32 > mesh.vert_count { return; }                // read
    let vbase = mesh.alloc.vbase;                                                          // read
    self.pool.update_verts(queue, vbase, verts);                                           // &self
}
```

- `gfx3d/pool.rs:488` is the whole of it:

```rust
pub fn update_verts(&self, queue: &wgpu::Queue, vbase: u32, verts: &[WgrMeshVertex]) {
    queue.write_buffer(&self.vbuf, vbase as u64 * VERT_SIZE, bytemuck::cast_slice(verts));
}
```

**Finding: `&mut self` on `mesh_update` is decoration.** Nothing structural is touched: no
slotmap insert or remove, no `bake_bind_cache` line, no `epoch` bump, no allocator call.
The exclusivity is the FFI boundary's, exactly as REN-THR-001 predicted. Three facts an
upload needs — `vbase`, `vert_count`, and the current vertex buffer — are all reads.

### `wgr_mesh_set_skin` — structural, and REN-THR-001 was wrong about it

The brief allowed for this and asked for what was found rather than what was expected.
What was found is that `set_skin` does **not** belong on a contents-only path.
`gfx3d/mod.rs:4613`:

- `self.meshes.get_mut(key)` — a **mutable** slotmap borrow (`mod.rs:4621`).
- `device.create_buffer_init(...)` and `mesh.skin = Some(...)` (`mod.rs:4634`) — it creates
  a **new GPU buffer** and stores it in the mesh record. That is an allocation table write,
  not a `write_buffer`.
- `self.bake_bind_cache.remove(&key)` and `bake_bind_cache_prev.remove(&key)`
  (`mod.rs:4644–4645`), with the comment saying why: *"A cached skin-bake bind would
  reference the old (or absent) skin buffer."*

The draw path reads that cache — `contains_key(&g.mesh)` at `mod.rs:4859`, `insert` at
`4895`, `get(&g.mesh)` at `4923`, with the per-frame swap at `4738`. So `set_skin` is in
the same class as `mesh_create`: it must not land while a frame is in flight.

It is also **not per-frame traffic**. `VertexBufferWgpu::SetSkinData`
(`EngineWgpu.cpp:679`) is guarded by `if (skinned || …) return;` and sets `skinned = true`
at its end (`:716`) — it runs **once per skinned buffer, at bind-pose upload time**. So the
argument that made `mesh_update` special (copying a vertex array every frame is the wrong
answer) does not apply to it at all. It is a creation-time call wearing an update-time name.

### `wgr_mesh_destroy` — structural, and yes, it touches `bake_bind_cache`

`gfx3d/mod.rs:4662`:

- `self.meshes.remove(key)` — slotmap removal.
- `self.pool.free(&mesh.alloc, vert_count, index_count)` → `pool.rs:496`, which calls
  `valloc.free` / `ialloc.free`: **the range allocator's free lists**, the tables
  `Pool::alloc` reads to decide where the next mesh lands.
- `self.bake_bind_cache.remove(&key)` / `bake_bind_cache_prev.remove(&key)`
  (`mod.rs:4671–4672`).

So the answer to the brief's question is **yes**, explicitly, at `mod.rs:4671`. A destroy
landing mid-frame drops a bind group the frame in progress may still be about to read at
`mod.rs:4923`, and returns pool ranges a later create can hand to a different mesh. Destroy
must be drained at a frame boundary; it is the same data dependency REN-THR-001 found for
create, arriving from the other end.

### `wgr_mesh_create` — as REN-THR-001 recorded

`mod.rs:4569–4578`: samples `pool.generation()`, allocs, and clears both bind caches when
the generation moved. Confirmed unchanged. Creation stays synchronous here and is out of
scope for this slice.

### The trap the audit was asked to establish: can an in-place update move an allocation?

**No — and the reason is worth writing down, because the surface below depends on it.**

`vbase` is stable for a mesh's whole life. Both events that can change the pool's backing
buffer preserve every live offset:

- **Growth** (`pool.rs:501` `alloc_verts`) allocates a bigger buffer and
  `copy_prefix`es the *entire old capacity* into it before installing it — offsets keep
  their meaning.
- **Shrink** (`pool.rs:381` `maybe_shrink`) only ever trims *trailing* free space:
  `shrink_target` returns `None` unless `alloc.trailing_free()` yields an offset, and the
  new capacity is `want.max(toff).max(init)` where `toff` is above everything live
  (`pool.rs:349–369`). Nothing live is below the cut, so nothing is relocated.

There is no compaction path in the pool at all. `MeshAlloc.vbase` is written once by
`Pool::alloc` and never rewritten.

**But the buffer OBJECT is replaced**, in both cases — `std::mem::replace(&mut self.vbuf,
new_buf)` at `pool.rs:406` (shrink) and `pool.rs:516` (growth), with the old one retired.
That is the real hazard for anything holding a cloned `wgpu::Buffer`: not a moved offset, a
**stale buffer**. The pool already has the tripwire and the comment for it — `pool.rs:33`
records the failure mode as *"Buffer with 'wgr_geo_pool_vbuf' label is invalid"* on submit.
So an off-handle uploader may cache `vbase`; it may **not** cache the buffer without a
republish rule.

The republish sites are exactly two in production code — `Pool::alloc` under
`Gfx3d::mesh_create` (`mod.rs:4570`) and `Pool::frame_submitted` under
`Gfx3d::frame_submitted` (`mod.rs:4606`). Every other `pool.alloc` / `frame_submitted`
match in the crate is inside `pool.rs`'s own tests. Two sites is a rule that can be held.

### Audit summary

| Call | Structural state touched | Verdict |
|---|---|---|
| `wgr_mesh_update` | none — `meshes.get` (shared), `Pool::update_verts` (`&self` + `Queue`) | **contents only; safe off-handle** |
| `wgr_mesh_set_skin` | `meshes.get_mut`, new `wgpu::Buffer`, `bake_bind_cache{,_prev}.remove` | structural — stays with creation |
| `wgr_mesh_destroy` | `meshes.remove`, `RangeAllocator::free` ×2, `bake_bind_cache{,_prev}.remove` | structural — must drain at a frame boundary |
| `wgr_mesh_create` | pool growth, both bind caches cleared | structural (already known) |

## Part 2 — what shipped

`WgrUploader`: a second FFI surface that is `Send + Sync`, reached by
`wgr_uploader_get(renderer)` once, and used thereafter **without forming `&mut *renderer`**.
It owns a clone of `wgpu::Queue` (internally refcounted, `Send + Sync`), the current pool
vertex buffer, the pool epoch that buffer belongs to, and a `handle -> (vbase, vert_count)`
table for meshes that can be uploaded through it.

- `wgr_uploader_mesh_update(uploader, id, verts)` — the per-frame path. Reads the table,
  bounds-checks against `vert_count` exactly as `Gfx3d::mesh_update` does, and issues one
  `Queue::write_buffer`. No renderer reference is formed anywhere in it.
- `wgr_uploader_mesh_destroy(uploader, id)` — pushes the handle onto a deferred list. The
  renderer pops the list and runs the real `Gfx3d::mesh_destroy` at the **top of
  `render_frame`**, before anything reads `bake_bind_cache` — which is what the audit's
  `mod.rs:4671` finding requires.
- The renderer republishes buffer + epoch into the uploader at the two sites named above,
  and only when the epoch actually moved.

`set_skin` did **not** move. The audit says it is structural and one-time, so it stays on
the renderer handle next to `mesh_create`, and the write-up says so rather than the code
quietly pretending otherwise.

### The lifetime contract, as documented in the header

Written into `wgpu_renderer.hpp` in the same voice the existing FFI uses for
null-tolerance, and repeated in `ffi.rs`:

1. The pointer from `wgr_uploader_get` is valid **until `wgr_destroy`** and is stable for
   the renderer's whole life. It does not need releasing; it is a borrow, not an owner.
2. It may be used from **any thread**, concurrently with itself.
3. It may **not** be used concurrently with a call that forms `&mut *renderer` — that is,
   with any other `wgr_*` entry point on the same renderer. Today that is trivially true
   because everything is serial; when a render thread exists this is the rule that says
   uploads belong to the producer and everything else to the consumer.
4. A handle passed to `wgr_uploader_mesh_update` after its `wgr_uploader_mesh_destroy` is
   **ignored**, not undefined: the destroy removes the metadata entry at enqueue time, so
   the update finds nothing and returns. Same null-tolerance discipline as
   `wgr_mesh_update`'s "unknown handles are ignored".

### What was deliberately NOT locked, and why — stated plainly

Everything is serial today. The uploader therefore ships **one** `RwLock` around its
published state and **one** `Mutex` around the deferred-destroy list, and no more. In
particular:

- **There is no lock coupling an upload to a concurrent pool realloc.** The republish rule
  makes the buffer correct at every point where a serial caller can observe it. A future
  render thread that could realloc the pool *while* the producer is inside
  `wgr_uploader_mesh_update` would need the write to be ordered against the realloc — the
  `RwLock` gives the necessary read/write exclusion for the buffer handle itself, but the
  *policy* question (does the producer stall, or does the realloc wait for in-flight
  uploads?) is not decided here and no speculative machinery for it was added.
- **Destroy-while-upload-in-flight is a contract, not a lock.** The metadata entry is
  removed synchronously when the destroy is enqueued, so a *subsequent* upload is a no-op.
  Two threads calling update and destroy on the *same handle simultaneously* is not
  defended against, because the contract above forbids it: mesh lifetime is producer-owned
  and single-threaded. This is written down so the next person changing it knows it is an
  assumption rather than an accident.

Adding locks for concurrency that does not exist yet would be untestable and would be
guessing at a design the thread slice has not made. The rule followed here is REN-THR-006's:
each slice is provable **serially** before there is any concurrency to blame.

## ABI

No hashed struct changed shape. `WgrUploader` is **opaque** — an incomplete type on the C++
side and a zero-sized `#[repr(C)]` marker on the Rust side, never dereferenced by either;
what crosses is a pointer. So neither hash list moved: `wgr_layout_hash` in `ffi.rs` and the
`WgrHashSize` list in `wgpu_renderer.hpp` are untouched, and `WGR_ABI_VERSION` stays at 8.
Three new exports, no version bump — which is the rule the brief states and the reason the
`WgrObjectStats` entry's comments exist.

## Verification

### Suites

Built and run in this worktree (`.claude/worktrees/agent-upload2`), preset
`win-x64-clang-rwdi`. **Nothing was deployed and the game was never launched** — another
session owns the game folder.

| Suite | Before (this worktree, change reverted) | After |
|---|---|---|
| `PoseidonTests` | 3,111 cases / 389,942 assertions / **0 failures** | 3,111 / 389,942 / **0** |
| `PoseidonCoreTests` | — | 838 cases / 9,808 assertions / **0 failures** |
| `PoseidonFoundationTests` | — | 491 cases / **0 failures** (160,753 assertions; this total varies by design) |
| `wgpu_renderer` (Rust, rwdi) | 216 passed / 1 ignored / 0 failed | **221 passed** / 1 ignored / 0 failed |
| `OpenPoseidon.exe` | links | links |

The brief quotes 389,947 assertions for `PoseidonTests`. This worktree measures **389,942
both with and without the change** — the five are a difference between checkouts, not
something this commit did. That is why the "before" column was measured here rather than
quoted: the change was reverted file-by-file, `PoseidonTests` rebuilt and rerun, and the
number came back identical. (`git stash` was **not** used for this: stashes are shared
across every worktree of this repo, and reaching for one nearly popped another agent's
`agent-heap-race: wip2` into this tree. It was restored; the stash entry survived.)

The five new Rust tests are the whole of the delta: construction and metadata bookkeeping,
an update's bytes reaching the pool at the right offset, an oversized update refused rather
than clipped, deferred destroy draining exactly once, and a real pool growth republishing.

### Ablation

Three, each reverted after measuring.

| Ablation | Caught by | What it printed |
|---|---|---|
| Write at offset `0` instead of `vbase * VERT_SIZE` | `an_update_reaches_the_pool_at_the_right_offset_and_leaves_the_neighbour_alone` | `mesh b did not receive the new vertices — left: 5000.0, right: 7777.0` |
| Off by **one vertex**: `vbase * VERT_SIZE + VERT_SIZE` | the same test **and** `a_pool_growth_republishes_and_uploads_keep_landing` | `post-growth upload did not reach the pool — left: 1000.0, right: 4242.0` |
| `take_pending_destroys` clones instead of draining | `a_deferred_destroy_drains_once_and_stops_further_uploads` | `a drained destroy must not be handed out twice` |

The first two matter for a specific reason: a wrong offset does not produce garbage, it
produces *plausible vertices in the wrong place*. Tagging vertex `i` of a mesh as `tag + i`
and asserting on both the target and its **neighbour** is what makes "correct" separable
from "corrupted something else" — a test that only checked the target would have passed the
off-by-one at every index but the last.

Full Rust suite re-run after the revert: 221 passed / 0 failed.

## What this does not prove

- **Nothing here proves thread-safety.** No second thread exists. What is proved is that
  the per-frame upload path no longer *requires* the exclusive handle, and that the serial
  behaviour is unchanged.
- **No in-game verification.** Another session owns the game folder; nothing was deployed
  and nothing was launched. The evidence is the suites and the unit tests, not a frame.
- **The republish rule is proved by inspection, not by a realloc test on a real device.**
  The Rust unit tests run against the headless harness, where a pool growth mid-run is
  possible to construct but a *shrink* needs the frame cadence.
- **It does not empty REN-THR-006's table.** `wgr_mesh_create`, `wgr_mesh_set_skin`,
  `SetFarProxyInstances`, surface/swap-interval calls and every stats reader still form
  `&mut *renderer`.
- **Deferring the destroy is a behaviour change, not a pure refactor.** A mesh's pool
  ranges now return to the free lists at the next frame's start rather than at the
  destructor. The only observable difference is that a create in the same frame cannot
  reuse those ranges yet, so peak pool residency can be marginally higher on a frame that
  destroys and creates heavily. Unmeasured in a real scene; the suites do not see it.

## What is left before a render thread can start

1. **`wgr_mesh_create` and `wgr_mesh_set_skin`** — both structural, both creation-time.
   REN-THR-001 already resolved the synchronous-handle objection to create (`Pool::alloc`'s
   only failure is empty input), so the remaining work is routing them through the drain,
   which slice 3 already does for the *retained* mesh path. The CPU path is the same shape.
2. **`SetFarProxyInstances`** — bulk, load-time, ~117 MB of source rows on Chernarus-size
   worlds. Needs a decision about ownership, not a queue.
3. **The reads.** Every stats reader, `HasGrassPhotoClumps`, and the grass placement
   witness. This is the snapshot problem and it belongs to the thread slice itself.
4. **`ResizeSurface` / `SetSwapInterval` / screenshots** — stop-the-world class, like
   device recreate.
5. **The simulation divergence** REN-THR-005 records (SIM-815 in flight) must be closed or
   bounded, because the equivalence oracle is what will decide whether a threaded frame is
   the same frame.
