# HANDOVER 2026-08-31 — the overnight roadmap run

Written while the owner slept, at their instruction to keep working and to save
after every milestone. Everything below is on `safety/asset-obs-001` and is
**deployed** — check `DEPLOYED-FROM.txt` in the game folder before trusting any
of it; the tip at the time of writing is what is installed.

## Read this first: what needs the owner's eyes

Nothing here is blocked on the owner, but three things are worth their attention:

1. **Mouse wheel is now a bindable input** (upstream backport `a9cdfc29`). Owner
   smoke: Options → Controls → Keyboard & Mouse → Map Zoom In/Out should show
   "Mouse wheel down/up" by default *on an existing profile too* (a v3→v4
   migration adds them without displacing custom keys); rebinding, swapping the
   two directions, and surviving a restart are the things worth trying.
2. **GL33 retirement: the recommendation is DO NOT.** Evidence doc says CI runs
   111 of 117 Trident scenarios on GL33 and **zero** on WGPU, so deleting GL33
   today deletes the only backend the automated suite can run. Owner decision,
   not an agent's.
3. **The night lit-ground artifacts under DLSS are still open** and were
   re-confirmed on the fixed build. Parked at owner's request; they are a
   separate defect from the soldier bug (chip `task_c060eaf7`).

## The soldier bug, for the record

Owner-reported "rifles floating in front of soldiers, soldiers half under the
ground" in campaign mission 1. It was **not** a regression from this branch —
yesterday's build shows it too, but **only with DLSS active**, which is why two
innocent commits were bisected onto and one was even reverted before the trigger
was found. Root cause and the bisect trap that cost the hours:
`docs/roadmap/decisions/REN-TEMP-002-...`. Fixed, owner-verified.

## What landed overnight

| Area | What | Evidence |
|---|---|---|
| Phase 5 | Snapshot **ring of 2** with a publish generation, a declared (unfed) resource-epoch seam, and a drop counter | 9 new unit cases; the four concrete blockers for the render thread are now written down in `render-snapshot-design.md` §3b |
| Phase 6 | ODOL adapter **tail split** — the worker conversion stops before proxies, the main-thread install runs proxies→OptimizeShapes in the required order; async adapt back on by default | 797 cold models converted on workers, main-thread adapt bucket **0.0 ms** |
| Phase 7 | GPU residency now **measured in bytes** beside the item count (`gpu_residency{}` in capture-metrics, Streaming-tab rows) | 119.9 KB/resident object ⇒ the 20,000-item budget promises ~2.3 GB; 661 MB of geometry-pool slack; the "budget" is a fixed 2 GB guess, not the device's VRAM |
| Phase 7 | **Bindless slot lease** — a slot belongs to a texture, not to one upload — composed with the submit-boundary retire | 5 new rendering tests; eviction genuinely exercised (18 evictions at `WGR_DYNAMIC_VRAM_MB=900`) |
| Phase 8.1 | **Worker FP environment** captured on main and applied to enkiTS + preparer workers | real hazard: with `enablePIII` the main thread ran flush-to-zero ON while every worker ran it OFF |
| Phase 8.2 | RNG **positional vs sequential** split, documented, with an off-thread guard | audit of 276 call sites: nothing off-thread uses the sequential stream today |
| Phase 8.3 | AI ordering fixes | a guarded-vehicle ranking sorted by **heap address**, and its comparator contained a **cycle** (ptrdiff narrowed to int); `CompUnits` claimed `a>b` and `b>a` for the same pair |
| Phase 2.2 | GL33 sole-renderer **gate evidence** | ~10.7 k GL33-only lines, only 6 live forks in shared code, 1 of 8 gate bullets satisfied |
| Tooling | `tri` now kills the game **process tree** on every abandoned spawn path, plus a Ctrl-C handler | this is what left the zombie game windows the owner saw |
| Tests | The eight long-standing engine failures are gone | the DLSS panel's six raw ImGui widgets now route through the search wrappers (a real find by that test); external-data tests skip when `packages/` is absent; the GraphicsPage AA test no longer asserts this machine's `graphics.cfg` |

Engine suite **235,122 assertions / 0 failures**; Foundation 159,556 / 0; renderer
192 / 0. All three were red or partly red at the start of the night.

## Verification actually performed

Every deploy was checked with the same two runs, both with **DLSS genuinely
active** (the NGX teardown crash after "Shutdown complete" is the witness that
DLSS initialised — an exit-0 run did not):

- campaign `00Training.Abel`, 35 s screenshot — the instructor must be visible
  with his rifle *in his hands* and a shadow;
- the 30 m/s Everon traverse with `--capture-metrics`, plus a **10-minute soak**
  (0 errors, clean shutdown, preparer queue drained, residency stable at
  19,984/20,000).

The campaign is extracted with a small packing-aware PBO reader written for this
(scratchpad `unpbo.py`); `dist/` needs `nvngx_dlss.dll` copied beside the exe or
DLSS silently falls back to native — that is the trap that produced the false
bisect.

## Open, in priority order

1. **Retained-texture liveness** — `_lastUsedFrame` is refreshed only by the
   direct-draw path, so a retained model's textures age out and are never
   refilled. This is why the pin check in `TrimResidency` must stay, and why
   buildings go white under artificial memory pressure (A/B'd: identical before
   the slot lease, so pre-existing).
2. **Real VRAM budget, then the byte budget.** Probing the device heap must come
   before any byte-driven admission gate, or the gate shrinks the resident set on
   a card with room to spare.
3. **Geometry pool slack** — 661 MB allocated and unused; doubling growth with no
   compaction.
4. **Phase 5 step 6 (render thread)** — four named blockers, the largest being
   that the *push* path (`_draws3d`/`_verts`/`_cmds`/`_palette`) is still
   single-buffered.
