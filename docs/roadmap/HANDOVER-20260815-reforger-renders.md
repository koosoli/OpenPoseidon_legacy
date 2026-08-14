# Handover — Arma Reforger's Everon renders in WGPU, with assets

**Date:** 2026-08-15
**Branch:** `codex/wgpu-performance-modernisation`
**Read first:** [ARF-001](decisions/ARF-001-reforger-pak-is-iff-and-two-of-its-layers-are-already-implemented-20260814.md),
[ARF-002](decisions/ARF-002-the-substituted-world-path-accepts-only-oprw-modern-20260814.md),
[DZ-008](decisions/DZ-008-dayz-water-never-dips-and-the-pond-material-was-the-wrong-file-20260814.md).

---

## HOW TO WORK THIS TASK — read before anything else

**Use subagents aggressively and in parallel.** Essentially every result below came from a subagent
with a tightly-scoped brief, and the work does not fit in one context. What made them productive:

1. **Give each agent exclusive file ownership** and name the files the *other* agents own. Collisions
   across `PakCommand.cpp` / `XobCommand.cpp` / `engine/WgpuRenderer/` are the main failure mode.
2. **State the verification bar as a number, not a goal.** "Report `taken / total`", "report mean RGB,
   global std and mean local 8x8 std against these baselines". Agents that were told what to measure
   found real defects; agents told to "make it work" reported success on things that did not work.
3. **Hand them the traps up front** (the list below). Every one of them cost hours the first time.
4. **Tell them to checkpoint findings to a file after every measurement.** Agents got cut off by
   transport errors and watchdogs repeatedly. A resumable agent that wrote nothing down loses
   everything; `SendMessage` to its id resumes it from its transcript.
5. **Expect them to correct you.** Four of my own claims were overturned by agents I briefed with
   those claims. That is the system working. Say so in the brief.

**Push only to the private mirror.** Remote `safety` =
`https://github.com/koosoli/PoseidonWGPU-private-safety-mirror`. Never `origin` / `poseidon-wgpu`
(public) or `upstream`. **No Reforger or DayZ data in the repo, ever** — generated worlds and models
live under `C:\Users\mail\AppData\Local\Temp\rf\` and the game directory.

---

## What works today

**Everon renders in WGPU with terrain, materials and objects**, converted from Arma Reforger's own
data. Run it:

```
cd "D:\SteamLibrary\steamapps\common\ARMA Cold War Assault"
$env:WGR_OBJECT_STREAM_MAX_OBJECTS=200000
.\ColdWarAssault.exe --render=wgpu --window --width=1600 --height=900 --dev `
  --test-mission "dev-missions\devtest-dawn.abel" `
  --test-world "C:\Users\mail\AppData\Local\Temp\rf\world\ev_DALL.wrp" `
  --test-world-freefly 5200 7000 220 45 -8 --test-world-hour 10 `
  --log-file cwr-everon.log --log-level info
```

Objects stream in over **10–20 seconds**; do not judge the first seconds. No `--addon-root` — models,
textures and materials are **loose files** under the game directory, which sidesteps the addon
product allow-list entirely.

### Every Reforger format is decoded and closes on the full corpus

| format | closure |
|---|---|
| `.pak` archive | 16/16 on all four criteria; 222,566 files |
| `.edds` texture | 99.3% of a stratified 1,692 sample (BC7/BC4/BC5 written this session) |
| `.emat` material | 10,308/10,312; 4 header forms; inheritance resolved (18.3% inherit) |
| `.terr`/`.ttile` terrain | 5,282/5,282 tiles; **10,308/10,308 shared edges byte-identical** |
| TMAT/BMAT blend mask | 23,123/23,123 payloads; 40,000/40,000 sub-cells |
| `EBIN` world | 103/103 files, 168,860,053/168,860,053 bytes |
| `.xob` model | 15,809/15,809 |

C++ readers: `engine/Poseidon/Asset/Formats/Enfusion/{EnfusionIff,PakArchive,TerrainTile,EbinWorld,XobModel}`.
Tools: `PoseidonTools pak inspect|list|worlds|terrain|export` and `xob inspect|convert|export`.

### The import pipeline

```
PoseidonTools xob export "<Reforger>\addons" -w worlds/Eden/Eden.ent -o C:\...\rf `
  -m C:\...\rf\everon_placements.txt --resmap-cache C:\...\rf\resmap.tsv --max-models 5000 --textures

PoseidonTools pak export "<Reforger>\addons" -w worlds/eden -o C:\...\rf\world\ev_DALL.wrp `
  --terrain-assets C:\...\rf\world\assets --terrain-vdir "reforger\everon\data" `
  --placements C:\...\rf\everon_placements.txt
```
then copy `assets\reforger` and the model tree into the game directory.

Staged results: **1,132 models, 99.0% prefab coverage, 1,216,138 objects**, 1.39 GB model tree.
The unresolved 1.0% is 562 GUIDs — 451 in no resource map, 111 with no mesh and no parent.

---

## The traps. Hand every one of these to every agent.

1. **A missing `.p3d` or texture is reported as a SUCCESS.** `ShapeBank::New` never returns null; a
   miss yields a zero-LOD shape every renderer skips silently. The placement loop's try/catch never
   fires and "N objects created" counts missing models as created. **Verify by pixels, never by
   that count.**
2. **`--screenshot-delay` counts GAMEPLAY FRAMES, not seconds** (`AppConfig.cpp:621-623`). A value of
   12 fires ~1 s into gameplay, before the 10–14 s of cache-miss ODOL decoding finishes. This
   invalidated a whole round of my captures and made a scale-up A/B look falsely flat. Use hundreds
   to low thousands.
3. **`RptF` is compiled out** (`DebugLog.hpp:61`), so every `LoadOprwModern` failure prints nothing.
   Absence of errors is not evidence of a load.
4. **`scripts\Build.ps1` exits 1 on a SUCCESSFUL build** — PowerShell 5.1 wraps native stderr as
   `NativeCommandError`. Use `cmake --build build/win-x64-clang-rwdi` directly.
5. **Never pipe or redirect `Start.ps1`'s output.** It closes the child's stderr and wgpu panics with
   `os error 232` and never draws.
6. **`Stop-Process -Name PoseidonGame -Force` kills OTHER AGENTS' sessions.** This demonstrably
   destroyed several runs. **Use PID-scoped kills.**
7. **The game-directory exe can be ABI-mismatched** with the DLL (`engine 4, renderer 5`). Deploy
   BOTH `PoseidonGame.exe` and `wgpu_renderer.dll`, or use `Start.ps1`, which runs the matched `dist`
   pair.
8. **`PoseidonTools config bin` writes the WRONG raP flavour for rvmats** (config flavour, version 4,
   source path embedded) and `ParseArmaRapMaterial` throws on it **silently inside a catch**. A real
   rvmat is version 0, `08`, an enum offset, root body at 0x10. There is a working material-raP
   writer in `PakCommand.cpp`.
9. **`WritePAA` accepts formats the PAA reader cannot read** — ARGB8888 fails entirely;
   ARGB4444/1555/AI88 give "LZW Decode error". Only DXT1/3/5 round-trip. 4096² DXT3 truncates its
   mip chain. **Always round-trip a written `.paa` back through the reader.**
10. **Alpha format decides cutout.** `ClassifyGpuSection` reads the *face texture's* alpha class:
    DXT1 → always opaque; DXT5 with soft alpha → Blend → **dropped from the retained pass**. Leaf
    cards need DXT5 with the top mip thresholded to hard 0/255.
11. **`uvTransform` is a basis, not row vectors.** `u = (aside[0], up[0], dir[0], pos[0])`,
    `v = (aside[1], ...)`. Putting the v scale in `up[2]` makes v identically zero and the whole
    world samples row 0 — it renders white and nothing says why.
12. **Do not name a world `eden.wrp`** — `TerrainWgpu.cpp:301` special-cases that exact name for a
    DayZ-era grass override.
13. `modelIndex` out of range is the loader's **only** hard failure (`Oprw25.hpp:662-663`) — it
    aborts the whole parse. Range-check every index written.

---

## Four times a check said "pass" while the thing was wrong

This is the most transferable lesson here, and it cost the most time.

* **`EbinWorld` byte accounting closed at 168,860,053/168,860,053 across 103/103 files while
  returning an EMPTY prefab reference for all 1,230,897 placements.** Closure constrains the
  *layout*, never the meaning. `:` is a decl token whose operand is the prefab; reading it as a bare
  token ended the record.
* **`.xob` LOD order.** A header comment said coarsest-first; it is finest-first. Selecting the
  minimum threshold yields a six-triangle billboard that parses, closes and round-trips **perfectly**
  while being the wrong mesh.
* **"The residency streamer stalls at `resident=128 pending=1`."** It never stalled. The logger only
  emitted on centre-change or convergence, so a fill in progress printed one line and went quiet.
  Stratis converges `128 → 19,998` in 2.5 s. I propagated this false claim into three agent briefs.
* **"`registered=31, main inst=0` — almost nothing reaches the draw path."** Measured on a stale
  binary. Rebuilt: 19,884 resident → `registered=19,892`, 8,522 drawn.

---

## Open work, in priority order

1. **Object performance.** 35.6 ms at 19,884 objects → 74.2 ms at 153,600 → 124.9 ms with all
   1.2M live. The populated view is ~13 FPS. **This is the blocker for playability.**
   The new timers exist for exactly this — see below.
2. **Grass and wind — UNFINISHED, an agent was mid-flight.** Reforger's clutter (`.conf` collections,
   `c_GrassAtlas_*_MASK.edds` as R8) is decoded but unused; the terrain still uses OFP clutter, and
   nothing sways. Its running notes are at `C:\Users\mail\AppData\Local\Temp\rf\wind_findings.txt`.
   Establish the WGPU vegetation-animation mechanism with `file:line` **first**; if sway data lives
   in a `.xob` field `XobModel` discards (it skips bones), report that rather than guessing. **Motion
   needs a different proof from a still image**: difference two frames at different frame numbers,
   with a wind-off control, since clouds and shadows drift on their own.
3. **Water GPU load — ANSWERED, and it is the water SURFACE DRAW, not the reflection.**
   A user reported 99% GPU facing water at a 60 FPS cap on a 4080. Measured on a 3070, Stratis open
   sea, 1080p, MSAA 4x: turning toward water costs **+16.56 ms of GPU per frame**.

   | | facing | averted |
   |---|---:|---:|
   | GPU frame total | 23.681 | 7.122 |
   | **Water draw** | **11.843** | 0.062 |
   | Planar reflection total | 1.995 | 0.000 (skipped) |
   | FFT sim | 0.838 | 1.295 |

   * **`aa24c32` genuinely fixed the reflection**: 1.995 ms at 1080p / 2.519 ms at 1440p at 85%
     coverage, in the policy's *most expensive* configuration, against 26.9–29.2 ms pre-policy.
     Coverage gating verified — averted, the pass is skipped outright.
   * **The limiter is not buggy.** `GameLoop.cpp:71-135` is a whole-loop blocking `Sleep()` after
     `Simulate()` (which encodes, submits and presents), computed from CPU wall time — so it bounds
     submission rate, not GPU execution. "99% GPU at a 60 FPS cap" is arithmetically just "GPU frame
     ≈ 16 ms", which is what was measured.
   * **The water draw is fragment-bound, linear in pixels**: 4.130 ms at 800x600, 11.843 at 1080p,
     19.336 at 1440p → **≈1.8 ms fixed + 4.7 ms per Mpixel**. Halving `GRID_N` would not touch it.
   * **Biggest lever: MSAA.** `WGR_MSAA=1` takes water draw 11.843 → **5.935 ms** (−49.9%) and the
     frame 23.681 → 16.257. The water pipeline has no alpha-to-coverage and no per-sample shading
     (`water/mod.rs:666,720`), so this is ROP/bandwidth from a *blended* full-screen pass into a 4x
     MSAA rgba16f target. Rendering water at one sample or at reduced resolution is worth ~half the
     water cost. **Not applied** — needs a separate resolve path.
   * Scaled to a 4080 (~1.7–1.9x fragment throughput): ~12–14 ms at 1080p, ~19–21 ms at 1440p, i.e.
     75–100% of a 16.6 ms budget. The report is reproduced with no limiter bug required.
   * **Free win available:** `lib.rs:2827` calls `water.update_interactions` unconditionally, outside
     every visibility gate — FFT + foam + interaction run every frame even facing empty sky (1.3 ms,
     19% of the averted frame). `spectrum evolve` is a pure function of time, not an integrator, so
     gating it cannot pop; foam and interaction *are* integrators and need care.

   Notes: `...\rf\water_findings.txt`.
4. **Only 50 of 1,132 models are textured.** `xob export --textures` did the first 50; the rest draw
   white. Also unwired: normal maps, and `MatPBRMulti` 4-layer blends (only `BCR_1` binds).
5. **Terrain exposure.** `ambient`/`diffuse` in the rvmat are **ignored** by the WGPU terrain path
   (1.0/0.55 and 0.1/0.1 give byte-identical captures); measured transfer from authored albedo to
   pixel is ~3.0x and compressive. A suspected exposure regression on Stratis versus `aa24c32`'s
   reference is worth checking.
6. **The DayZ pond defect** (DZ-008) is still open. The strongest lead: the 182 Chernarus pond models
   are exactly the population whose visual LOD names a DXT5 **normal map** in the stage-0 slot. Needs
   one frame capture on a clean tree.

### The performance instrumentation, and what it already says

Regions 33–45 in `rust/src/gpu_timers.rs` now time object cull, depth prepass (solid/cutout), colour
(solid/cutout), **each shadow cascade individually**, and the CPU direct path, with counters and a
selected-LOD histogram, surfaced through `--capture-metrics`.

Measured on Everon `00training`, 800x600: **GPU frame 13.724 ms, attributed 73.6%, residual 26.4%.**

| pass | ms | share |
|---|---|---|
| Shadow cascades (4) | 3.209 | 23.4% |
| Grass | 2.937 | 21.4% |
| Water | 1.989 | 14.5% |
| Object culling | 0.650 | 4.7% |
| Object draws (GPU-driven) | 0.075 | **0.5%** |

**This overturns the intuitive optimisation plan.** 76% of visible objects draw at LOD 0 or 1 — the
"detailed LODs stay active too far out" hypothesis is *confirmed* — and fixing it is near-worthless,
because the draws are 0.5% of the frame. Shadow cascades cost 0.805/0.801/0.806/0.798 ms while their
triangle counts span **48x**: cost is flat in geometry, so it is per-cascade fixed overhead, and
coarser per-cascade LODs would move nothing. Attack cascade count/resolution/pass sharing. Object
culling costs **8.7x the draws it produces**. The 3.63 ms residual is sky/atmosphere, cloud
composite, HDR resolve, tonemap/bloom/godrays and UI — none ever timed.

Also fixed: `GameApplication.cpp` declared `float timings[32]` against a 33-region contract, so
`WGR_GPU_TIMER_TERRAIN_COLOR` was **silently missing from every capture ever written**.

---

## Scratchpad reference (not in the repo)

`C:\Users\mail\AppData\Local\Temp\rf\` — `world\*.wrp` (`ev_DALL.wrp` is the full one),
`reforger\everon\{models,textures}`, `everon_placements.txt`, `resmap.tsv` (51,452 GUIDs),
`scaleup_results.txt` (381-line ledger), `residency_findings.txt`, `wind_findings.txt`,
`water_findings.txt`, `cap\*.png`.

Python prototypes under the session scratchpad `...\82098ab9-...\scratchpad\`: `pak.py`, `ttile.py`,
`blend.py`, `ebin.py`, `xob.py`, `resmap.py`, `rapmat.py`.
