# REN-ID-001 — Stable render-object identity (Phase 5.1 §4, blocker 3)

**Date:** 2026-08-31
**Branch:** `agent-render-object-ids` (from `safety/asset-obs-001` @ `3c7025ec`)
**Scope:** audit of the three identity schemes named in
`docs/roadmap/render-snapshot-design.md` §4, one design, and the slice that is verifiable
without a render thread.

---

## 0. Headline

**Two of the three schemes were already fixed before this session started.** The design
document still describes all three as open, because it was written before ID-1 and ID-2
landed on 2026-08-30 and was never revised. Implementing from the document rather than the
code would have rewritten working code twice over.

| # | Scheme (per §4) | State on arrival | Landed by |
|---|---|---|---|
| 1 | Raw `Object*` keys in `_gpuInstances` / `_gpuProxies` | **Open** — still raw pointer keys | — |
| 2 | 16-bit draw-sort id / motion-vector history key | **Done** — `Object::RenderId()`, monotonic, never reused | `62c0ac3b` ID-1 |
| 3 | Rust retained-instance free list | **Done** — generation-checked handles | `52b512d3` ID-2 |

Both are ancestors of this branch's HEAD (`git merge-base --is-ancestor` confirms).

`_drawObjectIds`, named in §4 and in `EngineWgpu.hpp:663-672`, **no longer exists** — grep
returns nothing. That field name in the roadmap is stale.

---

## 1. The audit

### Scheme 2 — the draw-object / motion-vector id: DONE, and not by generations

- **Created:** `Object::RenderId()`, `engine/Poseidon/World/Scene/Object.cpp:1744-1757`.
  Lazily assigned from a `static uint32_t nextRenderId`, skipping 0.
- **Stored:** `mutable uint32_t _renderId = 0` on the Object,
  `engine/Poseidon/World/Scene/Object.hpp:284` — beside `_gpuCoverage`, exactly where §4
  said the identity belonged.
- **Looked up:** `EngineWgpu::CurrentDrawObjectId()`,
  `engine/WgpuRenderer/EngineWgpu.cpp:6436-6449`.
- **If the object dies:** the id dies with it. There is nothing to clean up, and
  `SceneObjectRemoved` no longer erases anything for it
  (`EngineWgpu.cpp:6457-6458` says so in a comment).

**It is not generation-checked, and it does not need to be.** A monotonic never-reused
counter and a `{index, generation}` handle solve the same problem two ways: generations
exist to make a *recycled* index distinguishable, and this index is never recycled. Adding
generations here would be a second convention for no gain.

**What is genuinely still open in scheme 2** is not identity but *width*. The wire lane in
`WgrDraw3D.misc` is 16 bits, so `CurrentDrawObjectId()` folds the 32-bit id with
`(RenderId() % 0xFFFFu) + 1u` (`EngineWgpu.cpp:6448`). Two **live** objects 65,535 ids apart
alias one motion-vector history slot. The code says so in a comment and calls it bounded by
object-creation volume. That is a temporal-artefact bound, not a correctness hazard, and it
is fixed only by widening the ABI lane — a separate, larger piece of work.

### Scheme 3 — the Rust retained-instance free list: DONE

- **Created:** `instance_add`, `engine/WgpuRenderer/rust/src/gfx3d/cull.rs`. Returns a
  handle `(slot + 1) | generation << 24`, not a raw slot.
- **Validated:** `instance_slot(handle)` compares the handle's generation against
  `generations[slot]`; a mismatch returns `None` and increments `stale_instance_ops`.
- **Bumped:** `instance_remove` does `generations[slot] = generations[slot].wrapping_add(1)`
  *before* recycling the slot, so every outstanding handle dies at the moment of removal.
- **ABI:** `wgr_instance_add`'s contract is documented as ABI v7 in
  `engine/WgpuRenderer/include/wgpu_renderer.hpp:1907-1917` — "the returned u32 is an opaque
  HANDLE (generation-checked on the Rust side), not a raw slot index. 0 = failure,
  unambiguously."
- **If the object dies:** a handle held past removal resolves to nothing and is counted.

I looked for the failure mode that would make this a paper fix — a `clear()` or `truncate()`
that resets `static_instances` without resetting `generations`, which would desync the two
vectors and silently revert the check. **There is none:** the only writes to either vector
are the paired push in `instance_add` and the indexed bump in `instance_remove`. The
implementation is correct.

**What was wrong with it: it had no tests.** Not one test in the suite asserted that a stale
handle is refused. The existing tests only *decode* handles (`raw_slot`, cull.rs:2752) so
they keep working against GPU buffers that speak raw slots. Deleting the generation
comparison from `instance_slot` left the entire 208-test suite green. That is the gap this
session closed.

### Scheme 1 — raw `Object*` map keys: STILL OPEN, but reduced

- **Created:** `_gpuInstances[obj] = GpuInstance{...}` at `EngineWgpu.cpp:6344`;
  `_gpuProxies[parent] = std::move(set)` at `:6252`.
- **Declared:** `std::unordered_map<const Object*, GpuInstance> _gpuInstances`
  (`EngineWgpu.hpp:670`), `std::unordered_map<const Object*, GpuProxySet> _gpuProxies`
  (`:695`).
- **Looked up:** the admit guard `_gpuInstances.count(obj) != 0` (`:6281`),
  `SceneObjectRemoved` (`:6460`), `SceneObjectMoved` (`:6478`, `:6506`), the proxy refresh
  (`:6554`), the retained-liveness sweep (`:6023`, `:6033`), and the Culling tab dump
  (`:6599`).
- **Erased:** only via `SceneObjectRemoved` / `SceneObjectMoved`, which are called from
  exactly three sites: `Landscape::RemoveObject` (`Landscape.cpp:2211`), the cell unload in
  `LandSave.cpp:1292`, and the destruction-phase transition in `Dammage.cpp:499`.

**What happens today if the underlying object dies without one of those three calls:** the
map keeps a dangling key. Two things then follow, and it is worth being precise about which
is which, because the roadmap's framing overstates one of them.

1. **No use-after-free.** The sweep at `:6023` reads only `it->second` (a cached `pos`,
   `shape`, `mode`, `coverage` captured at registration) and uses `it->first` solely as a
   *key* into `_gpuProxies`. The dangling `Object*` is compared, never dereferenced. This is
   luck rather than design — nothing enforces it — but it is the current state.
2. **A recycled address inherits the dead entry.** A new `Object` allocated at the same
   address hits `_gpuInstances.count(obj) != 0` at `:6281` and is silently denied GPU
   registration for its whole life. Worse, its `SceneObjectMoved` / `SceneObjectRemoved`
   would drive `wgr_instance_update` / `wgr_instance_remove` **with the dead object's
   handle**.

**And that last step is where ID-2 already saves us.** Since 2026-08-30 that handle is
generation-checked, so the call is refused and counted instead of writing through to
whatever now occupies the retained slot. Scheme 1's worst outcome has therefore already
been downgraded from *silent corruption of a live instance* to *a leak*: one leaked map
entry, one `LODShapeWithShadow` reference that `ReleaseGpuModel` never releases, and one
object permanently denied the GPU path.

**With a render thread** the picture changes and property (1) stops being safe. A sweep
walking `_gpuInstances` concurrently with a main-thread `SceneObjectRemoved` would read a
rehashing table — a data race on the container itself, before identity even enters into it.
Scheme 1's real render-thread blocker is therefore **container ownership**, not key
validity; key validity is the part that becomes *cheap* to fix once the id exists.

---

## 2. The design

One identity, reusing what already exists rather than adding a third convention.

- **The id is `Object::RenderId()`** — a process-monotonic, never-reused `uint32_t`,
  resident on the Object. Already implemented, already the motion-vector key.
- **Generations live only where indices are recycled.** That is exactly one place: the Rust
  retained-instance table's slots. `Object::RenderId()` needs no generation because it
  recycles nothing. Two mechanisms, one for each shape of problem, and no third.
- **Who bumps:** `InstanceTable::remove` bumps the slot's generation. Nothing bumps a
  `RenderId`; it is assigned once and dies with the object.
- **What a stale id does when presented:** `InstanceTable::slot_of` returns `None` — the
  update or remove is dropped, never applied to the slot's current occupant — and
  `stale_ops` increments. The first eight also log to stderr with the handle, the slot, the
  presented generation and the current one.
- **The bound, stated rather than assumed:** 8 bits of generation. A stale handle aliases
  again only after 256 remove/add cycles *of the same slot*. This is now asserted by a test
  so the number cannot drift silently.

**For scheme 1**, the design is to store `RenderId()` in `GpuInstance` / `GpuProxySet` and
compare it on lookup: a recycled `Object*` presents a different `RenderId`, so the stale
entry is detected instead of inherited. That is one `uint32_t` compare on an already-hot
path. **It is designed here and deliberately not implemented — see §4.**

---

## 3. What was implemented (ID-3)

All in `engine/WgpuRenderer/rust/src/gfx3d/cull.rs`. No ABI change, no C++ change, no
behaviour change on any passing path.

1. **Extracted `InstanceTable`** from `CullState`: the slots, the free list, the
   generations, the stale counter and the dirty range, with `add` / `update` / `remove` /
   `slot_of` / `handle`. `CullState` keeps one `instances: InstanceTable` field and
   delegates; the five fields it replaces are gone.

   **Why the extraction was necessary and not tidying.** `CullState::new` builds compute
   pipelines, so it needs a device. Every test written against it therefore opens
   `let Some((device, queue)) = headless() else { return; }` — and **passes vacuously on a
   machine with no adapter.** The identity rules are pure bookkeeping; a test of them that
   can silently skip is not evidence. `InstanceTable` touches no wgpu, so its tests always
   run. (This is the trap recorded in memory as "Naga tests miss resource validity" and
   "check the readback isn't vacuous", in a new place.)

2. **Made the rejection observable.** `stale_instance_ops` was a write-only field: it
   incremented and printed eight times, and nothing could ever read it. It now has
   `InstanceTable::stale_ops()` and `CullState::stale_instance_ops()`. Without a reader, a
   test cannot tell a *rejection* from a *no-op that happened to leave the data alone*.

3. **`handle == 0` is now counted as well as refused.** Previously it returned `None`
   without incrementing the counter, so the FFI's error value passed through invisibly.

4. **Five tests**, none of which existed before.

### Behaviour-preservation note

`static_dirty` moved into `InstanceTable` as `dirty`, taken by `take_dirty()`. The
pre-existing code cleared it unconditionally at the end of `prepare`, *including* on the
path where `instance_buf.buf` is `None`. `take_dirty()` is therefore called before the
`if let Some(buf)`, not inside it, so the clear still happens on that path. This is
commented at the call site.

---

## 4. The ablation

The check ablated: in `InstanceTable::slot_of`, the generation comparison

```rust
Some(&current) if current == generation => Some(slot),
```

replaced with

```rust
Some(&_current) => Some(slot), // ABLATED: generation ignored
```

i.e. a handle resolves to its slot whenever the slot exists, which is precisely the
pre-ID-2 behaviour.

| Test | Ablated | Message |
|---|---|---|
| `a_recycled_slot_refuses_the_dead_occupants_handle` | **FAILED** | a stale handle wrote through to the live instance that reused its slot |
| `a_stale_handle_cannot_remove_the_instance_that_replaced_it` | **FAILED** | a stale handle removed the live instance occupying its slot |
| `handle_zero_is_never_a_valid_slot` | passed | guarded by the `+1` bias, not the generation |
| `a_handle_for_a_slot_that_was_never_allocated_is_refused` | passed | guarded by the `.get()` range check |
| `stale_handle_aliases_only_after_256_generations` | passed | exercises only live handles; documents the bound |

`211 passed; 2 failed` under ablation, `213 passed; 0 failed` restored. The three that pass
under ablation are testing *different* guards, which is the correct outcome — each of the
table's three refusal reasons is now separately pinned, and only the two that depend on the
generation comparison move when it is removed.

Two of the five tests also carry an explicit anti-vacuity assertion — they assert that the
slot was *actually recycled* (`raw_slot(dead) == raw_slot(live)`) before asserting anything
about the handle. Without it, a change to the free-list order would turn the test green by
never exercising recycling at all.

### Test counts

| Suite | Before | After |
|---|---|---|
| `cargo test -p wgpu_renderer --profile rwdi` | 208 passed, 1 ignored | **213 passed, 1 ignored** |
| `PoseidonTests.exe` | 240,905 assertions / 3,019 cases, 0 failed | **unchanged** (no C++ touched) |

---

## 5. Is blocker 3 closed?

**Reduced, not closed.** Honestly:

- **Scheme 2 — closed.** Identity is stable, Object-resident and never recycled. The 16-bit
  wire fold is a separate, known, commented ABI-width issue, not an identity one.
- **Scheme 3 — closed, and now defended.** It was correct on arrival but untested; it is now
  both. The 256-cycle aliasing bound is asserted rather than assumed.
- **Scheme 1 — open.** Still raw `Object*` keys. Its severity is already reduced from
  corruption to leak by scheme 3's check, but the keys themselves are unchanged.

### Why scheme 1 was not implemented in this session

Two reasons, and the second is the stronger one.

1. **It is not testable here.** `EngineWgpu` lives in the renderer library and is not linked
   into `PoseidonTests`; the only C++ test that mentions it is `test_zbias_math.cpp`, for
   unrelated maths. A `RenderId` validation added to `_gpuInstances` today would ship with
   no test that could fail — exactly the state ID-2 was in, and exactly what this session
   was called in to fix. Shipping a second untested check to fix an untested check is not
   progress.

2. **The identity fix is not the load-bearing half.** As §1 sets out, a render thread's
   first problem with `_gpuInstances` is that it is an `unordered_map` mutated on the main
   thread while a sweep walks it — a container race that a per-entry id does not touch. The
   id becomes worth adding *as part of* whatever gives that container an owner (blocker 4's
   push-path double-buffering is the natural place), not before it. Adding it now is
   machinery whose only consumer is a render thread that does not exist — the standing
   lesson this project has already paid for.

### What remains, in the order it should be taken

1. **Scheme 1's key validation**, together with `_gpuInstances`' threading story — not
   before it. The design is in §2 and costs one `uint32_t` compare; the work is deciding who
   owns the container.
2. **Surface `stale_instance_ops` across the FFI** into the dev panel's diagnostics (the
   `SnapshotDiag` / `StreamingDiag` pattern). The Rust-side reader now exists; the ABI
   addition does not. This is what would turn "scheme 1 leaks in theory" into a number
   measured on a real world — worth doing before, not after, the fix, so the fix has a
   denominator. It needs a game run to be worth anything, which this session was scoped out
   of.
3. **Widen the draw-object wire lane** past 16 bits to remove the motion-vector aliasing
   window in `CurrentDrawObjectId()`. ABI work, independent of the above.

### Correction owed to the roadmap

`docs/roadmap/render-snapshot-design.md` §4 and §3b item 3 describe all three schemes as
open and name a `_drawObjectIds` field that no longer exists. They should be revised to
match §0 of this record, so the next reader does not re-derive ID-1 and ID-2.
