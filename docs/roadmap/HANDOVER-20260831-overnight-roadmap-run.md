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

## Second half of the night (05:00-05:30)

Everything below is committed, pushed and **deployed** — the game folder is stamped with
the tip at the time of writing.

| Area | What | Evidence |
|---|---|---|
| Phase 7 | The GPU-cost item-budget clamp ships **opt-in** (`WGR_OBJECT_STREAM_GPU_BUDGET=1`) | Right where it matters — **0 interventions** at the probed 5,611 MB budget, so a set that fits is never shrunk. Wrong under pressure — at 1,200 MB it clamps 20,000 → 13,608 and then changes its mind **96 times in 200 s**, because shrinking the set changes the per-object measurement that drove the shrink |
| Phase 5.3 | The snapshot counters are **visible** at last — a section of the Fixed Step tab and a `snapshot{}` block in `--capture-metrics` | They had been written on every publish since the ring landed and were readable only from a unit test |
| Phase 5.3 | The drop counter is **not zero**, and the first capture that showed it said so | `firstDropGeneration == fallback + 1` in both of two runs: the one drop per world load is the menu→world handover frame. The counter now has a known-good value instead of an assumed one |
| Phase 3.1 | A capture now records **which binary and what resolution** produced it | The JSON recorded neither; the commit lived only in the bench script's console. First run with it also settled what the bench had been measuring: `3.05-f84ec7a-dev, render 2305x965 → output 3441x1440, msaa 4, dlss_active true` — not the 800×600 the roadmap warns about |
| Phase 3.1 | The `cpu_milliseconds` column was **dead in every capture ever written** — all 58 regions read −1 | `begin_frame` cleared the array the reader reads. Now published on frame end like the GPU rows; 13 of 58 regions report real time. Largest is 0.08 ms against a 25.6 ms frame, so recording the frame is not where the CPU's time goes. The existing test asserted the buggy contract and was rewritten |
| Phase 4.3 | **Audited: every clause already met** — and not by the mechanism the call site suggests | No explicit `Texture::destroy()` anywhere; handles are dropped and wgpu's tracker defers the free. The 120-frame eviction window is a thrash heuristic, not a safety margin — said at the call site now, replacing a stale NOTE describing an already-fixed bug |
| Phase 11A | `poSetMyPriority` was a silent no-op on Windows (thread ID cast to a HANDLE) | **Latent, not live** — the only Windows caller carried a workaround from the commit that introduced it. Fixed with a test that reads the priority back from the OS and that the old code fails |
| Phase 1.4 | **HRTF probe and modes** merged in from the audio agent | `ALC_HRTF_STATUS_SOFT` was never queried, so "user chose Off" and "driver refused" logged identically. Now distinguishable. Measured here: 1 dataset, `Built-In HRTF`, and Auto ≡ On on this device because HRTF is already on before anything touches it |
| Phase 8.6 | **Worker-count determinism harness** merged in from the determinism agent | Terrain generation is bit-identical at serial/1/2/4/8/32 workers. The negative control found a latent bug nothing had exercised: regenerating into a reused segment appended a second copy of the mesh (571 → 1056 → 1541 → 2026 vertices) and kept a stale water table |

### 05:30-06:00

| Area | What | Evidence |
|---|---|---|
| Phase 4.3 | **Audited: every clause already met**, and not by the mechanism the call site suggests | No explicit `Texture::destroy()` anywhere; handles are dropped and wgpu's tracker defers the free. The 120-frame eviction window is a thrash heuristic, not a safety margin |
| MAT-054 | **The converter now deploys a colour-only material** | The deploy gate was "an albedo was baked". Reforger's baked-LOD materials are `MatPBRBasic` with one flat `Color` and no map — the engine has synthesised a procedural colour from that since DZ-002 and never got the chance, so twelve materials fell to the 1.0 white at bindless slot 0. **The owner's deployed corpus is unchanged until a re-export runs** |
| 5.1 / sky | **The sky eased four times faster on a fast machine** | `alpha = 0.1` per FRAME makes the settling time a frame count: 0.33 s at 30 fps, 0.083 s at 120. Now a time constant, tuned so 60 fps is unchanged to three decimals. A steady clock, not the snapshot's float mission clock, whose representable step decays to 2 ms after eight hours of play |
| Phase 8.4 | **Physics boundary audited; no restructuring warranted** | The backend is box3d (not Bullet), `workerCount` measures 1, physics steps once per fixed tick. But the negative controls found two real bugs in paths nothing calls: `RemoveBody` left a destroyed body in `_probes` (measured SIGSEGV), and `CastRay` used generation parity as a liveness test while slots are reused without re-bumping — so after any removal the next occupant of that slot reported "no body" for its whole life, and that answer feeds shell penetration |
| Phase 1.4 | **EFX item 2 turned out to be 25 years old and already rejected** | Four of the five classifications shipped in *Operation Flashpoint* in 2001; the fifth and the crossfade landed 2026-08-30 and were parked hours later behind `POSEIDON_EXPERIMENTAL_EAX=1` after the owner's verdict. The agent found one checkable thing wrong inside it: interiors ran at a modal density of 0.02–0.08 against the 0.4287 the ROOM preset ships with |
| Diagnostics | **The same defect, three times in one night** | The dead `cpu_milliseconds` column; the snapshot drop counter whose documented correct value was 0 when the truth is one per world load; and my own new `per_object_kb`, which reported a flat 0 on missions that do not use object streaming at all. Worth naming: **a diagnostic that cannot say "did not run" will be read as "ran"** |

### 06:00-06:20

| Area | What | Evidence |
|---|---|---|
| Phase 2.1 | **Eight more freeze reference scenes**, GL33 and WGPU from the same camera | Gate bullet 6 said two scenes "is not important visual reference scenes; it is the first two entries". Town, water, combat, field, all pairs, in `docs/roadmap/evidence/gl33-freeze/`. The water pair states the capability gap plainly: WGPU draws grass, volumetric cloud and a coloured sea with a shoreline gradient where GL33 draws none, a flat sky and flat grey water |
| Phase 2.2 | **The compat gate bullet finally has a test that can fail** | 9 worlds across 4 generations and 5 OPRW revisions, plus one model PBO per generation and the whole installed Reforger `.pak` corpus. Proven by mutation: changing `Oprw25.hpp`'s version check left the ENTIRE pre-existing suite green and failed only the new file. It was possible to break the Arma 2 and DayZ world readers without one existing assertion noticing. **Still only PARTIAL** — this is asset compatibility; it draws no pixels, and the bullet sits in a renderer gate |
| Phase 6 | **Slice 3 should not be attempted**, and the table says why | `swap` is 27.171 ms of a 28.686 ms frame. All main-thread CPU work sums to ~1.5 ms and material interpretation is a fraction of the 0.081 ms `land:obj` bucket. There is nothing to reclaim; the frame is GPU-bound |
| MAT-054 | The converter fix was a **no-op on its first attempt**, and a measurement caught it | The widened deploy gate sat behind an early `return` in the `bcr.empty()` branch: a 400-model Everon export reported 21 materials with "no BCRMap and no BCR_N layer" and **zero** colour-only deploys. MAT-054 had named the right branch; I took the "equivalently" alternative it offered, and the two are only equivalent if control reaches it. Moved, re-run on the same 400 models: **451 .emat / 0 colour-only → 463 .emat / 11 colour-only.** The owner's corpus is unchanged — see the top of this document |

### 06:20-06:50 — the measurement block

| Area | What | Evidence |
|---|---|---|
| Phase 3 | **DLSS priced for the first time: ~1.07 ms** | 1072×603 → 1600×900 on the RTX 3070, as the difference between two arms that render the same internal pixel count. Invisible to the profiler because NGX's dispatches are not ours to bracket |
| Phase 3 | **~2.8 ms of every GPU frame is fixed overhead**, and it does not scale with work | Measured across four scenes: the residual is 2.7-2.9 ms whether the frame is 10.5 ms or 20.1 ms. ~65 µs across each of ~45 pass boundaries. **The lever is fewer, larger passes, not more brackets** — and because it is constant it grows as a share of every improvement made elsewhere: 13% of a heavy frame, 27% of a light one. This ranks **ahead of track B.4**: grass is the biggest *attributed* cost at 6.415 ms / 20.8%, but this block is bigger |
| Diagnostics | **Two timer regions explained more than the whole frame** | `Water draw` and `Post: god rays` were missing from the containment table, so a naive sum double-counted them — on `perf_water` the "sum of outermost" came to 20.485 ms against a 20.322 ms frame, a **negative** residual. Both open their own render pass on the encoder rather than sitting inside one, which is why they read as standalone; god rays was moved into the colour segment by a recent change and the table did not follow. Fixed, redeployed, re-measured |
| Phase 1.1 | **The upscaler boundary already existed** — a fourth already-done roadmap item | A vendor-neutral `TemporalUpscaler` trait with an ordered backend chain, from the DLSS work. What was genuinely missing: the orchestration recovered per-backend facts by **string-matching the backend's display label** in three places — §1.1's forbidden branching, spelled in a way the compiler cannot check. Replaced with a declared capability struct. **DLSS verified still genuinely active afterwards** (`route true, active true, quality 2, 1072×603 → 1600×900`, plus the NGX teardown crash as witness) |

Engine suite **240,660** assertions / 0 failures; Foundation **159,282** / 0; renderer
**208** / 0 (212 with `--features dlss`). All three green.

Two corrections were issued against my own work in the same session, and both are in the
history rather than amended away: `97b08a39` corrects `869ce872`, which claimed the
thread-priority bug had a live impact it did not have, and `34289573` corrects the commit
immediately before it, which called a closed blocker open because I read the design
document instead of the code — in a commit whose whole point was that the design document
had gone stale.

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
