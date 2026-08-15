# Handover — far-field tier, LOD governor, and the streaming transient

**Date:** 2026-08-15
**Branch:** `feat/farfield-tier` (worktree `.claude/worktrees/farfield-tier`), pushed to `safety`
**Parent:** `research/large-world-farfield` → `codex/wgpu-performance-modernisation`
**Read first:** [RND-035](decisions/RND-035-the-lod-governor-was-costing-40-percent-of-the-frame-20260815.md)

---

## 0. How to work on this — not optional

**Use subagents, aggressively and in parallel.** Nearly every result below came from one,
and the two largest findings of the session came from agents contradicting the brief they
were given. Concretely:

- An agent told to measure a 5-arm A/B noticed the **35 s warmup lands mid-stream** and ran
  the whole matrix twice. Without that, the wrong default would have shipped with a
  straight face.
- An agent asked to fix "transparent stones" proved the far tier could not be the cause
  (it was drawing nothing) and traced it to a *different branch's* texture edit.
- An agent benchmarking `getenv` caught its own measurement error — clang had CSE'd the call
  out of the loop, reading 2 ns instead of the true 2.1 µs.

Practical rules learned the hard way:

- **Give each agent disjoint file ownership** and say who else is editing. This worktree had
  up to four agents at once; the only near-miss was two agents wanting `EngineWgpu.cpp`.
- **Tell them not to deploy while another is benchmarking.** A mid-run binary swap
  invalidates every repeat.
- **Ask explicitly for null results.** "Report honestly if a change shows no improvement"
  produced the far tier's most important finding (it does *not* reduce fill-in pop).
- Read-only audit agents are extremely high value. The CPU/VRAM audit found `FrameProfiler`
  had been recording the answer all along and nobody was reading it.

---

## 1. State: 17 commits, all revertible from one binary

Every behavioural change is env-gated with the NEW behaviour as default, so an A/B needs no
rebuild:

| lever | default | legacy |
|---|---|---|
| `WGR_LOD_GOVERNOR_RANGE` | 4 | 16 |
| `WGR_OBJECT_STREAM_MS_PER_UPDATE` | 4 | 0 |
| `WGR_EVICT_ON_MODEL_RELEASE` | off | 1 |
| `WGR_FAR_TIER` | on | 0 |
| `WGR_SHADOW_LOD_PROP_CACHE` | on | 0 |
| `WGR_DRAW_SECTION_ENV_CACHE` | on | 0 |
| `WGR_RESIDENCY_CANDIDATE_CACHE` | on | 0 |

**Measured (settled Everon, identical residency, cheapest steady frame):
30.33 → 50.34 fps, GPU frame 32.9 → 18.9 ms, 56.0M → 9.1M triangles.** Entirely
`WGR_LOD_GOVERNOR_RANGE`.

---

## 2. Traps that will cost you a day each

1. **Warmup.** Residency needs **~81 s** on Everon (1.2M placements) to reach `pending=0`.
   Grep the log before quoting any number. At 35 s the arms *invert*.
2. **Degenerate captures.** A capture taken before streaming completes can be the cheapest
   frame in its arm with **zero** main instances. `farfield-compare.py` now rejects those;
   do not remove that guard.
3. **Deploy BOTH binaries** — `ColdWarAssault.exe` *and* `wgpu_renderer.dll`. ABI is 6; a
   mismatched pair fails the handshake or dies with `Entry Point Not Found`.
4. **Shared worktree.** `codex/wgpu-performance-modernisation` is another agent's branch
   with uncommitted work. Stage only your own files.
5. **One-shot GPU timer regions persist stale values.** Only `GPU frame total` is
   re-measured every frame.
6. **`land:gnd` is not terrain.** `UpdateModernObjectResidency` runs inside
   `TerrainWgpu::DrawTerrain`, so object admission bills to the terrain phase.

---

## 3. What is true now that was not before

- **The frame is CPU-bound only while streaming.** Settled it is 94.9% GPU. RND-032's
  "GPU is 8.9% of wall clock" is a transient, not steady state. This reverses the advice to
  target the CPU object pipeline on a settled world.
- **`land:gnd` is 206.6 ms streaming, 0.432 ms settled** — a 478× drop while resident
  objects grow 10×. It scales with *objects created per frame*, at ~1.1 ms each.
- **Everon VRAM is not oversubscribed** (`overBudget=0`, 1.36 GB of 2 GB), and of that,
  textures are 218 MB against 1.14 GB of geometry pool — where `capacity` exceeds `live` by
  335 MB because the pool doubles and never shrinks.
- **36.2% of placements inside the fog radius are drawn by nothing** at the coarse governor
  rail with a fixed far-tier cutoff. That is the pop-in, quantified
  (`experiments/farfield/cutoff_analysis.py`).

---

## 4. Next, in priority order

### 4.1 Verify the governor win on Arma 3 and DayZ — IN FLIGHT
An agent is exporting Stratis and Chernarus (both source games are installed under
`D:\SteamLibrary\...`) and running the governor A/B. **If a world's legacy LOD histogram is
not pinned to LOD 0/1, the governor was never its problem there** — that null result is the
most useful of the three.

### 4.2 The streaming transient — the headline problem
~1.1 ms of CPU per object created × 200k objects. Chernarus has 2.97M. Unattributed within
itself; prime suspects:
- `TextureWgpu::EnsureUploaded` builds the **full mip chain** and
  `TextureSourcePac::GetMipmapData` **re-opens the file per mip level**
  (`Pactext.cpp:1970-1974`). A 2048² PAA re-upload is ~11 VFS opens + 11 LZO decompressions.
- The synchronous ODOL/MLOD parse in `ShapeBank::New`, which recurses into every proxy.
- I/O cannot move off the main thread (`QBStream` `MT_SAFE` is 0), but **decode can**.

Cheapest first: cap mip levels during streaming, then deactivate-to-cache instead of destroy
on recentre (today a recentre destroys objects *and* their GPU textures — flying back
re-decodes everything).

### 4.3 Far tier complement rule — IN FLIGHT
Replaces the distance cutoff with "accept exactly when the real cull would drop". 0% overlap
and 0% gap at both governor rails, by construction. Needs no ABI change — `Gfx3d::cull_inputs`
already holds the coefficients.

### 4.4 Known-open, lower priority
- Far proxies render **white**: MLOD carries no average colour. A `colorKnown` flag marks the
  default rather than hiding it. Needs a colour source (sample the dominant texture at load).
- **See-through granite** — `granite_05_bcr_ca` has 24.1% dithered zero-alpha and MAT-047's
  FLAG tagg routes it to alpha-cutout. The 100%-clear case is guarded; this partially-clear
  case belongs with MAT-047 on the other branch and needs a perimeter/speckle metric.
- Reforger granite renders **near-black** in WGPU, correct grey in GL33. Pre-existing.
- `DrawSectionTL` still does a second `_materialBindings.find` via `MaterialIsReflective` and
  three `canonical()` heap allocations per section per draw. Needs a per-material memo; it
  changes binding order, so it needs a visual check.
- **`WGR_DRAW_SECTION_STATS=1` has never been run.** The `getenv` fix is µs-per-call × an
  unknown call count. Run it before crediting that change with anything.
- The Rust model tables are **append-only with no unregister** (`cull.rs:790-814`), and every
  registration re-uploads all five tables. Cost grows with lifetime registrations.

---

## 5. Tooling built this session

- `scripts/farfield-bench.ps1` — non-interactive capture. `--duration` is **server-only**;
  `--capture-metrics` only writes after a screenshot event. That is why naive runs produce
  no JSON.
- `scripts/farfield-compare.py` — cheapest-repeat selection, CPU phase split, CPU−GPU as the
  headline, rejects zero-instance captures, shows `-1` as "did not run".
- `experiments/farfield/` — isolated crate (own cargo workspace, links nothing from
  Poseidon). `cutoff_analysis.py` computes far-tier overlap/gap against real placements with
  exact per-model bounds; `mkplacements.py` dumps a world's placements + model table.
- `cpu_frame_phases_ms` in the capture JSON — the 12-phase CPU split, which `FrameProfiler`
  had been recording unread since long before this work.
