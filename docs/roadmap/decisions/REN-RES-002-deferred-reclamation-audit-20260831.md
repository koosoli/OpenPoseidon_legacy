# REN-RES-002 — Deferred reclamation: what actually makes destruction safe here

**Date:** 2026-08-31
**Branch:** `asset-obs-001`
**Roadmap:** Phase 4.3 — *"Resource destruction must be deferred until no published
snapshot, worker job or in-flight GPU submission can still reference the resource. Use
explicit generations/epochs or equivalent completed-work tracking rather than arbitrary
frame-count delays."*

## Why this is an audit and not a refactor

The obvious reading of 4.3 against this codebase is alarming. `TrimResidency` destroys
textures on a **120-frame grace window**, and 4.3 names arbitrary frame-count delays as
the thing not to do. A session that trusted that reading would rip out a working
heuristic and replace it with a fence it did not need.

The reading is wrong, and the point of writing this down is that the reason is not
visible from the call site. Measured, not assumed:

## What was checked

### 1. Nothing in the renderer explicitly destroys a GPU resource

```
grep -rn "\.destroy()" --include=*.rs engine/WgpuRenderer/rust/src/
→ no matches
```

wgpu exposes two ways to release a resource. `Texture::destroy()` / `Buffer::destroy()`
invalidate the allocation **immediately** and are the unsafe-if-in-flight route. Simply
**dropping** the handle is the other, and it hands the resource to wgpu's tracker, which
keeps the allocation alive until every submission still referencing it has completed.

This renderer uses only the second. `Textures::destroy` (`textures.rs:696`) removes the
entry from the registry — `map.remove(...)` — and drops it. `mesh_destroy` and the
geometry pool's shrink do the same. So the deferral 4.3 requires is present; it is
performed by wgpu's resource tracker rather than by our code, which is why it is invisible
to a reader auditing our code for it.

### 2. The one thing wgpu cannot defer for us — and we do defer it

A bindless **slot index** is not a resource; it is a number baked into material buffers
and into already-encoded draws for the rest of the frame. wgpu knows nothing about it, so
recycling it mid-frame would repoint live draws at a different texture. Both pools handle
this explicitly:

- `Textures::destroy` parks the freed slot in `bindless_retired`, and `frame_submitted()`
  — called immediately after the frame's `queue.submit` — is the only place it moves to
  `bindless_free` (`textures.rs:719`).
- `GeometryPool` reallocates inside the same submit boundary (`gfx3d/pool.rs`).

That is completed-work tracking keyed on an actual submission, not on a frame count.

### 3. Snapshots hold no resources to invalidate

`PresentationSnapshot` carries only scalars and by-value light/rotor records — no handles
— which is documented at `resourceEpoch` in `PresentationSnapshot.hpp`. So the "no
published snapshot can still reference the resource" clause is satisfied vacuously today.
The epoch field exists as the declared producer end of that seam for when it stops being
vacuous; nothing bumps it, deliberately.

### 4. Worker jobs hold no GPU handles

The `ObjectStreamPreparer` workers parse files and run `ShapeAdapter` conversion. Every
GPU-visible resource is created on the main thread during install. Nothing a worker holds
can be freed under it.

## What the 120 frames actually are

A **thrash heuristic**: how long a texture must go unused before we are willing to pay to
decode and upload it again. The consequence of getting it wrong is a wasted upload, not a
use-after-free.

Stating that at the call site is the only change this audit produced, and it replaced a
stale `NOTE` that described a bug — geometry bytes being tested against a texture-only
budget — which had already been fixed in the code directly above it. A comment describing
a fixed bug is worse than no comment: it sends the next reader to re-fix it.

## Gate status

| 4.3 requirement | Status |
|---|---|
| Destruction deferred past in-flight GPU submissions | **Met** — via wgpu's tracker; no explicit `destroy()` anywhere |
| Slot/index reuse deferred past the submit boundary | **Met** — `frame_submitted()` in both pools |
| Deferred past any published snapshot | **Vacuously met** — snapshots carry no handles |
| Deferred past worker jobs | **Met** — workers hold no GPU handles |
| Not relying on arbitrary frame-count delays for lifetime | **Met** — the frame count governs *policy*, not lifetime |
| Parallel-job mutation applied at an ownership boundary | **Met** — worker output is installed on the main thread |

## What this does NOT cover

- **Device loss.** Nothing here says what happens when the device is lost and every handle
  becomes invalid at once. That is what `resourceEpoch` is for, and it is unfed.
- **The GL33 backend.** Audited for WGPU only. GL33 has its own texture lifetime and was
  not examined; it is fallback-only and its retirement is a separate open decision.
- **Any claim that the 120-frame value is well chosen.** It is safe at any value; whether
  it is the right trade is a per-corpus measurement nobody has made. `WGR_TEXTURE_EVICT_GRACE_FRAMES`
  exists for exactly that experiment.
- **A test.** The safety property lives in wgpu, so a unit test here would be testing wgpu,
  not us. The property that *is* ours — slot recycling waiting for submit — is covered by
  `unleased_slots_are_still_recycled` in `textures.rs`.
