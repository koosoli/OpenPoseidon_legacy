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

| stage | models | model tree | coverage | `.wrp` | objects |
|---|---|---|---|---|---|
| A | 50 | 19.1 MB | 78.3% | 31.0 MB | 19,644 |
| B | 200 | 118.1 MB | 95.8% | 100.5 MB | 1,177,900 |
| C | 500 | 342.0 MB | 98.2% | 102.3 MB | 1,207,337 |
| D | **1,132** | **1,392.4 MB** | **99.0%** | 102.9 MB | **1,216,138** |

Every model round-tripped through `MLODLoader`; every `.wrp` re-read with the runtime's own reader,
so no `modelIndex` was ever out of range.

**What actually bounds the picture is the 20,000-object streaming budget, and it is a tunable.**
`WGR_OBJECT_STREAM_MAX_OBJECTS=200000` moves resident 19,884 → 153,600 and yields a frame
**pixel-equivalent to loading all 1.2M** (172.0/169.4/145.4 in both; local 8x8 18.59 vs 18.57) at 60%
of the GPU cost. Past ~150k resident there is nothing further to gain at that camera. Object capacity
itself was never reached: eager mode built 1,216,009 objects and drew 41,420 instances /
**297,553,823 triangles** in one frame without crashing.

**Cost, which is the real problem:**

| objects in file | resident | registered | drawn inst | local 8x8 | GPU frame |
|---|---|---|---|---|---|
| 0 (control) | 8 | 8 | 8 | 7.03 | — |
| 19,644 | 19,644 | 19,652 | 3,787 | 7.81 | 35.6 ms |
| 98,161 | 20,000 | 20,007 | 7,323 | 9.04 | 16.9 ms |
| 392,632 | 19,991 | 19,999 | 7,244 | 13.31 | 28.4 ms |
| 1,216,138 | 19,884 | 19,892 | 8,522 | 16.76 | 35.6 ms |
| 1,216,138 (budget 200k) | 153,600 | 153,592 | 19,531 | 18.59 | 74.2 ms |
| 1,216,138 (all live) | 1,216,009 | — | 41,420 | 18.57 | 124.9 ms |

So the populated view is **~13 FPS**. Ledger: `...\rf\scaleup_results.txt` (381 lines).

### The residency fix (uncommitted detail is in the diff; here is the reasoning)

`LandSave.cpp:2336-2339` built the candidate window from a **fixed `_modernObjectRadius` of 64 land
cells, applied *before* the budget test** — a second, unreported bound. On any world whose placement
database already fits the budget, that radius is the one that binds. Everon is 256x256 land cells at
50 m, so 64 cells spans 6.4 km of a 12.8 km world: **9,176 of 19,644 placements (46.7%) requested
with 10,824 of 20,000 budget slots unused.** Half the island was empty for want of nothing.

The radius is now a *starting* size that doubles until the budget binds or the window covers the map
(`LandSave.cpp:2335-2361`). **No cap was raised** — a bound was stopped from pre-empting the cap
already chosen. Everon `window=64 requested=9176` → `window=256 requested=19644`, converging to
19,644 in 2.2 s at **92.2 MB → 92.2 MB, 0.0 MB retired**: 2.14x the objects for no measurable GPU
cost, because they reuse resident models. Stratis is provably unchanged (the growth loop is never
entered where the budget already binds). A moving camera converges at every phase with `refused=0`.
`WGR_OBJECT_STREAM_WINDOW_GROWTH=0` restores the old behaviour for A/B.

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
2. **Wind and clutter.** Full notes: `C:\Users\mail\AppData\Local\Temp\rf\wind_findings.txt` (689
   lines, self-contained).

   **The finding: the WGPU renderer does not animate vegetation MODELS at all. Only grass.**
   There is one wind authority, `World/Weather/WindModel.hpp:100-137` (`GWind` at `:142`), a closed
   form in conditions/overcast/time. Grass consumes it (`EngineWgpu.cpp:5377-5384` →
   `grass/grass.wgsl:37-38`, `sample_wind_field` `:427-455`, blades `:975-989`), as do clouds and the
   FFT ocean. **Both object vertex shaders have no time-varying term whatsoever** —
   `gfx3d/gpu_driven.wgsl:362-487` and `gfx3d/shader3d.wgsl`. Two plausible-looking leads are not
   wind: `ForestPlain::Animate` is a static terrain conform (`ObjectClasses.cpp:401` says so:
   *"Forests and terrain don't move, so this is computed once"*), and `tree_bend` is a lighting knob
   that blends the leaf normal toward a radial crown normal (`Engine.hpp:915-917`). So sway had to be
   **built**, not hooked up.

   **Sway is built and measured working** on OFP pines (Malden), needing **no per-vertex channel** —
   only model-space Y, since this vegetation is authored origin-on-ground. That makes the "does
   `XobModel` discard the sway data with bones?" question moot. Against a `POSEIDON_VEG_SWAY=0`
   control at an identical camera:

   | region | wind on >8 | control >8 | on >16 | control >16 |
   |---|---|---|---|---|
   | canopy interior (leaves) | **7.82%** | 2.97% | **4.64%** | 0.66% |
   | sky only (drift baseline) | 3.41% | **5.10%** | 0.60% | **1.26%** |

   Leaf pixels change 2.6x more (>8) and 7.0x more (>16) while the sky in the same frames changed
   *less* — and the control ran 10.1 s of elapsed time against the wind-on pair's 6.6 s, so it is
   conservative.

   **Reforger vegetation does NOT sway yet — root cause found, fix written, NOT measured. Do not
   report it as working.** Reforger `.p3d` carry no named properties: **90 of 94 have no `map`
   token**, so `MapType = MapHide` and they receive no canopy flag — hence no sway *and* no canopy
   normals. That is a pre-existing defect this work merely exposed. A narrow name/path fallback in
   `ShapeLOD.cpp` (requires a `vegetation` path segment *and* a `t_`/`b_` prefix) compiles but never
   got pixels; `runmodel.ps1` is written and ready to finish it.

   **Everon grows NO clutter at all** — verified directly, not inferred: `0 surfaces, 0 characters,
   0 clutter classes`, `0 / 65536 surface cells, 0 renderable`, bare ground in the capture. So the
   task is "make any clutter grow", not "swap OFP's for Reforger's". Three blockers, all in the
   import lane: `BuildSurfaceCatalog` finds configs via PBO bank prefixes but Everon ships loose
   files; `ReadClutterCard` hard-requires ODOL `.p3d` and Reforger ships `.xob`; the fix belongs in
   `PakCommand.cpp` / `XobCommand.*`.

   Two claims in that area remain **unverified by anyone**: that Reforger's clutter `.conf` is a
   readable `Clut` format, and that `c_GrassAtlas_*_MASK.edds` is R8. Nobody has opened one.
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
   white. Re-run it with a higher `--max-models` — the mechanism works and needs no new code.

   How it resolves, since it is not obvious: the **MLOD face texture slot**, not the material slot.
   Per part, read the `.emat` from the `.pak`, run `ResolveEmatInheritance` (18.3% of Reforger
   materials get their maps only from a parent), take `BCRMap` as albedo and `OpacityMap` as the
   cutout mask (falling back to `BCR_1` for `MatPBRMulti`), decode both `.edds`, fold opacity into
   alpha, downscale to 1024, write `.paa`, **round-trip it back through the PAA reader**, and name it
   in the face texture slot. Result: `UNTEXTURED` sections 32 → **0**; 68/68 materials resolve an
   albedo; 85/85 written parts name a texture. Frame-wide means barely move because the objects cover
   0.24–0.42% of the frame, but every changed pixel went white → green-brown (greenness +1.79 →
   +15.02, luma −63).

   Still unwired: **normal maps** (Reforger's `NTCMap`/`NMOMap` are not plain tangent normal maps),
   and **`MatPBRMulti` 4-layer blends** — 2 of 68 materials, `GraniteCliff_01` (9,445 placements) and
   `t_picea_abies_stump_01` (8,275), bind only `BCR_1`. A material-side route would need `BCRMap`/
   `NMOMap`/`OpacityMap` added to `TranslateEmat`'s key list in `EmatMaterialAdapter.hpp`, and would
   still not give the cutout, which comes from the face texture.
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

> **SUPERSEDED (2026-08-15, later the same day).** The paragraph above concludes that object draws
> are 0.5% of the frame and that fixing LOD selection is "near-worthless". That was measured on a
> **13.7 ms frame**, i.e. a far lighter object load than the full Everon asset set at
> `WGR_OBJECT_STREAM_MAX_OBJECTS=200000`. At that load objects dominated the frame completely and the
> LOD fix was worth **6.5x**. The measurement was not wrong; its scope was. Keep the residual/cascade
> findings, discard the "near-worthless" conclusion. Numbers below.

### Object LOD: the A/B, measured

Every model exported by `XobCommand` carried **exactly one LOD**, so nothing could ever be selected —
`Objects LOD histogram (main view): lod0=19388` with every other bucket zero. `XobCommand` now
exports every valid graphical LOD. Both captures below use the same mission, world (`ev_DALL.wrp`),
camera, hour and budget, and — the part that makes it an A/B rather than two runs — **identical
streaming residency at capture time (153,600 resident)**. Reconstruct the invocation by reading it
back out of the baseline's own log, not from notes; `C:\Users\mail\AppData\Local\Temp\rf\caplod.ps1`
does exactly that.

| | baseline `99f0ff7` | multi-LOD `4b33b5e` |
|---|---|---|
| **GPU frame total** | **77.259 ms** | **11.850 ms** |
| Objects: prepass solid | 28.345 | 0.234 |
| Objects: colour solid | 16.625 | 0.672 |
| Shadow cascade 3 | 11.143 | 0.216 |
| main-view triangles | 115,962,974 | 575,807 |
| far-shadow (c3) triangles | 54,268,775 | 401,319 |
| LOD histogram | lod0 = 100% | lod0 = 1, bulk lod4 |

`GPU frame total` is the only figure here that is re-measured every frame; per
`gpu-timer-regions-persist`, one-shot regions persist stale values in `--capture-metrics`.

**Export correctness is verified, not assumed.** A census parsing all 1132 models in both trees
(byte accounting closed, 1132/1132 parsed in each): 1117 models gained LODs, mean 4.90; the 15 that
did not had single-LOD sources. LOD0 is **bit-identical** — SHA-1 over the point table, normal table
and every face record matches on all 1132 — so the finest LOD was not disturbed. Total LOD0
triangles 8,197,099 in both trees; all-LOD 14,943,272 in the new one.

**Two conclusions that look right and are not**, both retracted here after measurement:

1. *"The rank ladder is too compressed."* Native Arma models use **the same linear ladder**.
   `model inspect` on Arma 3's `t_pinuss2s_b_f` (pine) and `cargo_house_v1_f` (building) both give
   exactly **4 visual LODs at resolutions 1, 2, 3, 4**, everything else >= 10000 (special). That is
   precisely `VisualLodResolution(rank, base) = base*(rank+1)`. Native A3 also collapses to a
   **13-face** pine and a **76-face** building at resolution 4 — stub meshes at distance are the RV
   convention, not an export defect. Do **not** "fix" the ladder into a geometric one on the strength
   of `Tracks.cpp`'s `lodCoef *= 2`; that is a different mechanism.
2. *"The coarse trees are an artifact of the 800x600 capture."* Plausible — `cull.wgsl` picks the
   coarsest LOD whose `resolution <= dist * lod_inv_width * Camera::Left()`, and
   `Scene.cpp:785` sets `_lodInvWidth = lodCoef*2/Width()`, so a narrow window really does make LODs
   switch ~2.4x nearer than at 1920. **Tested and refuted:** at matched streaming state
   (registered ~82k) the histograms are effectively identical — 22.2 vs 25.9 tris/instance, lod4
   dominant in both, lod0 ~ 0%. Most instances are *clamped* at their model's coarsest rung, so
   shifting the switch distance does not un-clamp them.

What is actually true is duller and more useful: the aerial reference camera views objects
600-3000 m out, where `dist * k` runs ~8-40 — past the coarsest rung for **any** model, native or
converted. Cone trees in that frame are correct LOD behaviour. **The baseline's 116 M triangles was
the defect**, holding full LOD0 detail out to 3 km.

**The LOD path itself was never broken — only the XOB producer was.** Native ODOL content spreads
across buckets normally, and the buckets sum exactly to `main inst` in each case:

| world | format | histogram | lod0 share |
|---|---|---|---|
| Takistan | Arma 2/OA ODOL | 75 / 118 / 303 / 151 / 506 / 325 | 5.1% of 1478 |
| Stratis (1500 m) | Arma 3 ODOL | 208 / 160 / 1144 / 3926 / 305 / 955 | 3.1% of 6698 |
| Everon (before) | XOB-converted | 19388 / 0 / 0 / 0 / 0 / 0 | 100% |

So there is nothing to port to the other compatibility layers: they read real ladders and always did.

**Open / not yet closed.** A ground-level near-field quality capture is still owed — the aerial pose
cannot answer it, because almost nothing in that frame is within 200 m. Partial evidence from a run
that was killed mid-capture: shadow cascade 0 carried 183 instances / 428,864 triangles = **2,343
tris per instance**, between the reference spruce's LOD0 (3,216) and LOD1 (1,188), which indicates
near-field detail is intact. Finish that capture before calling the fidelity question closed.

**Operational trap worth knowing:** the shared "stop PoseidonGame PIDs that did not exist before my
launch" cleanup used by every capture script **kills other agents' concurrent runs**, because a run
started after another agent's snapshot looks new to it. Two captures died this way. Serialise timing
runs, or the GPU numbers are meaningless anyway.

---

## Scratchpad reference (not in the repo)

`C:\Users\mail\AppData\Local\Temp\rf\` — `world\*.wrp` (`ev_DALL.wrp` is the full one),
`reforger\everon\{models,textures}`, `everon_placements.txt`, `resmap.tsv` (51,452 GUIDs),
`scaleup_results.txt` (381-line ledger), `residency_findings.txt`, `wind_findings.txt`,
`water_findings.txt`, `cap\*.png`.

Python prototypes under the session scratchpad `...\82098ab9-...\scratchpad\`: `pak.py`, `ttile.py`,
`blend.py`, `ebin.py`, `xob.py`, `resmap.py`, `rapmat.py`.
