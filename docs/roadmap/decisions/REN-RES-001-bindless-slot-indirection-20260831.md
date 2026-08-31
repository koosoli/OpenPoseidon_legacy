# REN-RES-001 — A bindless slot belongs to a texture, not to an upload

Date: 2026-08-31. Unblocks the "baked bindless slot" argument written at
`TextureMipBias()` in `engine/WgpuRenderer/TextureWgpu.cpp` and the
`ResidentPins()` note in `TextureBankWgpu::TrimResidency()`, both of which
correctly identified this as a Rust-side blocker and left it open.

Scope: this closes the **correctness** half. The pin check in `TrimResidency`
stays, for a reason measured here and stated in full below.

## The problem, precisely

`wgr_model_register` resolves each material's texture handle to a dense bindless
slot **once**, at registration — `gfx3d/mod.rs register_model` →
`SharedTextures::texture_slot` → `SectionMaterialGpu.texture_slot` — and stores
the integer. `gpu_driven.wgsl` indexes the array with it
(`textures[sm.texture_slot]`). There is no `wgr_model_unregister` and no
material-update entry point; `cull.rs`'s tables are append-only. **A registered
model's slot index is final.**

So destroying a texture did two wrong things at once:

1. **The model went white forever.** `EvictGpu` clears `_uploadTried`, the
   re-upload gets a fresh handle *and a fresh slot*, and the baked material never
   learns the new number.
2. **The model could come back wearing a stranger's texture.** `destroy` pushed
   the freed slot onto a LIFO free list that `alloc_slot` pops *first*, so the
   next upload took the evicted texture's slot. Not a rare race — the guaranteed
   outcome, and silent.

(2) is the worse defect, and it is invisible: nothing logs, nothing asserts, the
building simply has the wrong picture on it.

The **direct** draw path has none of this, because `gfx3d/mod.rs` re-resolves
`texture_slot(d.texture_id)` during material staging every frame. That asymmetry
is exactly why unpinned textures were already safe to evict.

## The two candidate shapes

### (a) Stable id + a per-frame id→slot table the shader dereferences

Models store a stable texture id; the fragment shader reads an indirection table
to turn it into a slot. Fully general — slots may be reshuffled freely — and it
is how a compacting allocator would have to work.

**Rejected on cost, in the hot path.** The dereference is per *material fetch*,
not per draw. `SectionMaterialGpu` carries up to **13** slot fields (albedo,
specular, normal, mask, 3 layers, 3 layer normals, …), so a fully-featured
material pays up to 13 extra storage-buffer loads per fragment, on the lit-mesh
pass, which is already the frame's dominant cost on stock worlds (grass and
object shading dominate; see the GPU-frame notes). The loads would be
cache-friendly — the table is a few KB and every wave reads the same entries —
but "probably absorbed by L1" is not a thing to assert about the hottest shader
in the renderer without measuring, and the benefit over (b) is only slot-space
compaction, which is not a resource under pressure (below).

### (b) The slot is stable for the texture's lifetime — **chosen**

A slot is **leased** by one *logical* texture — one `TextureWgpu`, i.e. one
texture *name* — rather than by one upload of it. While leased it is never
recycled. Eviction points the slot at the white fallback but leaves it reserved;
the re-upload reclaims the **same index**. Models never learn a new number
because the number never changes.

- **No shader change. Zero per-fetch cost.** `gpu_driven.wgsl` is untouched.
- **Both defects go.** (1) because the re-upload lands in the same slot; (2)
  because a leased slot is never handed to anyone else.
- **Cost: slot-space fragmentation**, and it is affordable by a wide margin. The
  high-water mark is the number of distinct texture *names* ever loaded, **not**
  the number of evict/re-upload *cycles* — a lease is held once and reused. The
  measured worst case in the corpus is DayZ Chernarus at **3,552** distinct
  object textures against an `object_cap` of **8,192** (`lib.rs`). Reforger
  Everon needs 218 MB of textures in total. There is no world here that runs out.

Running out is handled anyway rather than being a cliff: `slot_acquire` returns
0 at cap, and `create_in_slot(…, 0)` is exactly `create` — today's per-upload
behaviour. Degradation, not failure.

## What was implemented

Rust (`textures.rs`, `lib.rs`, `ffi.rs`):

- `SharedTextures::bindless_leased`, and `destroy` no longer recycles a leased
  slot — it blanks it to white and leaves it reserved.
- `slot_acquire` / `slot_release` / `create_in_slot`.
- FFI: `wgr_texture_slot_acquire`, `wgr_texture_slot_release`,
  `wgr_texture_create_in_slot`. `wgr_texture_create` is unchanged, so every
  existing caller keeps working.

C++ (`wgpu_renderer.hpp`, `TextureWgpu.*`):

- `TextureWgpu::_slotLease`, acquired lazily at the first upload, released **in
  the destructor only**. Both `EnsureUploaded` upload sites go through
  `wgr_texture_create_in_slot`. `EvictGpu` is unchanged — that is the point.

Releasing the lease on *eviction* would reintroduce defect (2) exactly; the
lifetime rule is stated at every one of the three places that could get it wrong.

Behaviour with nothing evicted is unchanged: an unleased slot is still recycled
(pinned by a test), and the bindless array stays as dense as it was.

## Why the pin check in `TrimResidency` still stays

Slot identity was **one** of two blockers. The other is **liveness**, and it is
not fixed here.

`_lastUsedFrame` is refreshed only inside `EnsureUploaded()`, which the *direct*
draw path calls every frame via `UseMipmap` — so a direct-draw texture's eviction
costs one white frame and self-heals. **A retained model's textures are touched
at registration and never again.** Nothing calls `EnsureUploaded` for them
per-frame.

Remove the pin test today and every retained texture ages out 120 frames after
registration whether or not it is on screen, is evicted, and is **never
re-uploaded**. The slots would all be correct and the buildings would all be
white — a worse bug than the one being fixed, and one that would look like a
success in any test that only checks slot bookkeeping.

What remains, therefore, is a per-frame "this registered model drew" signal plus
a refill request. That lives in `EngineWgpu`'s snapshot consumption, which this
change deliberately does not touch.

## Tests

Five headless tests in `textures.rs`, all of which **render through the real
`bindless_bind()` bind group** with a fragment shader that does
`textureLoad(textures[slot], …)` — asserting on `texture_slot()` alone would only
prove this module's own bookkeeping, not what the GPU sees.

- `leased_slot_survives_evict_and_reupload` — the central invariant. A slot is
  captured *before* eviction (standing in for what `register_model` bakes) and
  never refreshed; it must show the texture, then white, then the texture again.
  Includes a pre-eviction assertion so the test cannot pass vacuously.
- `evicted_slot_is_never_inherited_by_another_texture` — an unrelated upload
  between eviction and re-upload must not take the leased slot, and the model's
  slot must read white, **not** the stranger's colour.
- `unleased_slots_are_still_recycled` — no regression in density.
- `released_lease_returns_to_the_pool` — including a double release.
- `zero_lease_degrades_to_plain_create` — cap exhaustion degrades.

They skip (return) on an adapter without binding arrays, matching
`gfx3d::cull::tests::headless`.

## Verification status

`cargo test -p wgpu_renderer --profile rwdi`: **181 passed, 4 failed, 1 ignored**.
The 4 failures are **pre-existing on trunk** (`6d00e70b`) and unrelated to
textures — `gpu_timers::region_indices_stay_ffi_stable` and three
`sky::cirrus_puffiness_tests`. Verified by stashing this change and re-running:
clean trunk gives 176 passed / 4 failed / 1 ignored, i.e. this adds 5 passing
tests and no failures.

C++ builds clean (`PoseidonGame`, 626/626, links).

**Not verified in-game.** Another agent holds the single allowed game instance,
so no run was made. What still needs an in-game check is stated in the handover:
a memory-pressure run with `WGR_DYNAMIC_VRAM_MB` lowered far enough that the LRU
actually evicts, confirming buildings keep their textures rather than turning
white or swapping.

## Verified in-game 2026-08-31 (main agent, after integration)

Composed with the submit-boundary retire that landed separately (`d4dfdf4f`): a slot
now has two rules and they stack — the LEASE decides whether a slot ever returns at
all, the RETIRE decides when an unleased one may. `destroy()` parks an unleased slot
in `bindless_retired` (reusable at the next `frame_submitted()`), and skips a leased
one entirely.

**Eviction was genuinely exercised**, not assumed: `WGR_DYNAMIC_VRAM_MB=900` on the
30 m/s Everon traverse produced 18 `Wgpu texture LRU: evicted=` rows. Buildings render
**white** in that run — and an A/B against the pre-lease build (`aa2eea3e`, same flags,
same route) shows the identical white buildings, so this is the pre-existing liveness
gap this note already names (`_lastUsedFrame` is refreshed only by the direct-draw
path, so a retained model's textures age out and nothing ever asks for a refill), NOT
a regression from the lease. At the default 2,048 MB budget nothing goes white.

That A/B is the reason the pin check must stay until the liveness signal exists: the
lease makes eviction *safe to attempt*, it does not make the retained path *ask for
its textures back*.

## Liveness (2026-08-31, second change — closes the blocker above)

The pin check is now **relaxed**, gated on a liveness signal that did not exist when the
section above was written. Both things the retained path lacked are supplied.

### 1. The signal: `EngineWgpu::MarkRetainedModelLiveness()`

Called from `NextFrame` immediately **before** `TrimResidency()`, since the LRU is its only
consumer. It walks the retained instance table (`_gpuInstances`) and, for every instance
within the object draw distance of the camera, calls `TextureBankWgpu::NoteTextureUse` on
its model's textures — the same call `EnsureUploaded` makes on the direct path, so
`_lastUsedFrame` finally means the same thing on both.

**What was rejected, and why.** The honest signal is "which models survived the GPU cull".
The renderer does read cull results back, but only as *aggregate* counters
(`wgr_get_object_stats` / the `Objects counts:` row) — nothing is per model, and making it
per model is a Rust-side change. The CPU's own visible set is no better: `Scene::ObjectForDrawing`
diverts a **Full**-coverage object out of the draw list *before* the frustum test, so the CPU
never learns whether the objects most at risk — whole buildings — were on screen. What is left
is the instance table itself, and a cheaper question.

**Granularity, stated plainly:**

- **Per model, not per instance.** One instance in range marks the whole model's textures.
- **Distance only.** No frustum, no size cull, no occlusion — a strict *superset* of what
  drew. Deliberate: a texture wrongly kept costs VRAM, a texture wrongly evicted costs a
  white building. Turning round on the spot therefore evicts nothing, which is the intent.
- **Lag of up to one sweep pass.** The table is walked a slice of buckets per frame
  (`kRetainedLivenessSweepFrames` = 30) so a 200k-instance world costs a bounded fraction of a
  frame instead of one hitch. A rehash restarts the pass — restarting costs latency, skipping
  would cost correctness.
- Margin on the draw distance is 1.15x (`WGR_RETAINED_LIVENESS_MARGIN`), because the draw
  distance is measured to an object's centre while the cull works on its bounds.

### 2. The refill: inside `InvalidateStaleGpuModels()`

Reused rather than replaced, as the mechanism was already the right one — it just had one
outcome. It now **refills first and destroys second**: on a handle mismatch, a model the sweep
says is in range gets `EnsureUploaded()` on the missing texture, and only a model whose
re-upload *failed* is torn down. The recorded handle is then updated to the new one, which is
sound **only** because of the lease — the assertion being made is "the slot behind this handle
is the slot the material baked". It is therefore done only for a texture with
`SlotLease() != 0`; a texture that never got a lease (bindless array at cap) really has moved
index and still goes down the destroy path.

### 3. The relaxation, and the two failsafes on it

`TrimResidency`'s `ResidentPins() != 0` exemption is now conditional on
`_retainedLivenessActive`, which `EngineWgpu` sets only after a **complete** sweep pass. Before
that, a retained texture's `_lastUsedFrame` is still the frame it was registered on, and
evicting on it would be evicting on noise — the exact 900 MB failure above.

Two ways the signal can silently stop being trustworthy, both closed:

- **Marks stale because the sweep is slow.** A pinned texture must be unmarked for
  `max(grace, 2 × sweep period)` frames, not just `grace`. Without this,
  `WGR_TEXTURE_EVICT_GRACE_FRAMES=2` would condemn a model merely waiting its turn in the sweep.
- **Passes stop completing.** If the last *completed* pass falls further behind than that same
  age (heavy streaming rehashing the table over and over), the flag is withdrawn and the pin
  exemption comes straight back.

`WGR_RETAINED_LIVENESS=0` pins the whole signal off, so both arms of the A/B come out of one
binary — the discipline `WGR_EVICT_ON_MODEL_RELEASE` already follows.

**Why this cannot thrash.** An evict/refill loop would need a live model's texture to be
evicted. A live model is marked every pass, so its `_lastUsedFrame` is always fresher than the
grace, so it is never a candidate. When the in-range working set alone exceeds the texture
budget the LRU simply runs out of candidates and stops — the same over-budget behaviour as
before, with the buildings in view still textured.

### Tests

**None added, and the reason is structural, not an omission.** `PoseidonTests` links
`Poseidon` / `PoseidonGL33` / `PoseidonOpenAL` and **not** the wgpu backend
(`tests/unit/engine/Poseidon/CMakeLists.txt`), so no C++ unit test can reach `EngineWgpu`,
`TextureBankWgpu` or `TextureWgpu` at all. Everything new here is either a device-touching
call or a private member of those classes. Making it testable means either linking the
backend (and with it the Rust cdylib) into the unit suite, or lifting the pass-freshness and
range predicates into a backend-independent header — a worthwhile follow-up, out of scope for
this change.

`PoseidonTests` was run as a regression gate regardless: **2,968 cases / 235,117 assertions /
0 failures**, unchanged in substance from the base (the suite cannot link this code, so it
cannot be affected by it).

### What must be verified in-game

The same run that produced the white buildings, because it is the only one that exercises
this at all: **`WGR_DYNAMIC_VRAM_MB=900`, the 30 m/s Everon traverse.** Success is *both* of:

1. `Wgpu texture LRU: evicted=` rows still firing — the row now carries `(pinned=N)` and
   `liveness=1`. `liveness=1` with `pinned>0` is the proof the new path is doing the work;
   `pinned=0` means the LRU is still only reaching direct-draw textures.
2. Buildings keep their textures throughout, including after flying away from a village and
   back — the return is what exercises the refill, and
   `Wgpu model residency: N evicted textures re-uploaded into their leased slots` should
   appear when it happens.

A useful third arm from the same binary: `WGR_RETAINED_LIVENESS=0` at 900 MB must reproduce
the old white buildings. If it does not, the run is not exercising eviction at all.
