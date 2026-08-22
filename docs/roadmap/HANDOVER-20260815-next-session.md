# Handover — 2026-08-15, WGPU compatibility layers: what landed, what is still broken

**Branch:** `codex/wgpu-performance-modernisation`  **HEAD:** `4b33b5e`
**Nothing below is committed.** All code changes are uncommitted in the working tree.
**The worktree is shared with other agents.** Never `git add -A` or `git commit -a`; stage only files you own.

---

## 1. Owner-reported defects — the actual work list

These come from the owner testing a build deployed 2026-08-15 10:11. **Treat this section as the
priority queue.** Nothing here is diagnosed unless stated.

### Arma Reforger / Everon — worst layer, regressed
- **Performance is horrible, worse than the other layers.** Arma 2 and Arma 3 maps load faster and run
  better. Something is specifically wrong with this layer.
- **Asset loading takes far too long.**
- **Many assets still untextured.**
- **Ground/terrain texture looks very low quality, as if the wrong LOD is bound.**
- **Forests cast no shadows.**
- **Forest trees are extremely low-poly even when the camera is very near them.**

### Arma 1 (Sara)
- Roads are **fixed** — confirmed good by the owner.
- **Buildings and trees do not cast shadows from every viewing angle**; shadows *pop in* depending on
  view angle.
- Performance is not great.

### Arma 2 (Takistan)
- **Roads render as untextured white lines.**
- **Roads are visible through mountains** — terrain does not occlude them. *The owner suggests
  comparing against the Arma 1 road path, which is now correct.*
- **Normal maps appear to move/swim** ("drunk" effect) — paved brick surfaces shift around.

### Arma 3 (Stratis)
- **Light sources render as huge orbs resembling suns.** Long-standing bug; likely something
  interpreting a light as a sun/sky body.
- **Many building textures look wrong.**

### DayZ (and probably Arma 1 / others)
- **Tree canopies do not render correctly, especially at distance.**
- **Trees do not cast shadows on each other.**
- **Roads are not correctly drawn.**
- **Building textures look wrong, same character as Arma 3.**

### All layers
- **There is no far LOD for anything.** At distance there are no trees and no objects — only terrain.
- **Grass does not match environment lighting**; far too bright at night.
- **Grass self-shadowing appears not to work at all.**
- **Load the map's own default grass by default**, but keep our improved grass logic.

### Ocean
- **Coastline foam is too strong and forms visible repeating patterns when viewed from a distance.**

---

## 2. What landed this session (all uncommitted)

| area | change | evidence |
|---|---|---|
| Object LOD | `XobCommand` exports every graphical LOD, not just the finest | [RND-032](decisions/RND-032-single-lod-xob-export-cost-6-5x-of-gpu-frame-20260815.md) |
| Alpha cutout | `PAAEncoder` writes the `FLAG` tagg | [MAT-047](decisions/MAT-047-paa-without-a-flag-tagg-disables-alpha-cutout-globally-20260815.md) |
| Water | skip FFT/ripple/foam when no water is selected | [RND-033](decisions/RND-033-cross-layer-frame-profile-objects-are-not-the-bottleneck-20260815.md) |
| Cloud shadows | span sized from draw distance + edge fade | [CLD-021](decisions/CLD-021-cloud-shadow-map-ended-in-a-hard-line-at-2048-m-20260815.md) |
| Captures | cinema border suppressed for the dev free-fly camera | [RND-034](decisions/RND-034-the-capture-sub-rect-was-the-cinema-border-not-a-sizing-bug-20260815.md) |
| Roads | already fixed earlier in `aa24c32`; amended the wrong diagnosis | [WLD-021](decisions/WLD-021-road-banding-is-a-wgpu-shading-bug-20260813.md) |

Modified files: `apps/tools/Tools/commands/XobCommand.cpp` (committed in `4b33b5e`),
`engine/Poseidon/Graphics/Textures/PAAEncoder.cpp`, `tests/unit/.../test_paa_encoder.cpp`,
`engine/WgpuRenderer/rust/src/{water/mod.rs,sky/mod.rs,lib.rs,shaders/frame.wgsl,godrays.wgsl}`,
`engine/WgpuRenderer/EngineWgpu.cpp` (material-coverage counters — keep them, they are the only way to
get a denominator for "how much is untextured"), `apps/cwr/Game/GameApplication.cpp`,
`engine/Poseidon/Graphics/Shared/SdlWindow.cpp`.

---

## 3. Measured performance picture — read before optimising anything

`GPU frame total` at `WGR_OBJECT_STREAM_MAX_OBJECTS=200000`, 800x600:

| world | GPU frame | objects' share |
|---|---|---|
| Everon (Reforger) | 11.85 ms | ~8% |
| Takistan (A2/OA) | 12.04 ms | ~10% |
| Stratis (A3) | 27.48 ms | 9.4% |
| Chernarus (DayZ) | 20.15 ms @ 20k resident | 9% |

**Objects are not the bottleneck on any layer.** Do not spend another session on object geometry.

**The two big ones:**
1. **~43-58% of every frame on every world is in passes with no GPU timer region at all** —
   main-view sky, volumetric cloud march/composite, bloom, godrays, tonemap, UI
   (`timestamp_writes: None`). This is the largest single item and it is an *instrumentation* task
   before it is an optimisation task.
2. **The frame is CPU-bound at scale.** At 200k objects the GPU is **8.9% of wall clock**
   (10.7 ms GPU vs 121 ms wall). The bottleneck is object streaming/residency, not rendering.

**A perverse interaction worth fixing cheaply:** `_lodInvWidth` is a *framerate* feedback loop
(`SceneDraw.cpp:1031-1161`, clamped to `[qualitySettings, qualitySettings*16]`). A CPU-bound frame
makes it strip GPU geometry detail — measured 12.95M triangles down to 0.42M — to save time that was
never being spent on the GPU. **This is very likely the cause of the owner's "forest is low-poly even
when very near" and "no far LOD for anything" reports.** Feeding it GPU frame time instead of wall
clock is a small, principled change. Verify it against the owner's symptom before assuming.

Layer-specific costs: **Stratis → water 28%** (draw 2.97 + planar reflection 2.98 + FFT/foam 1.67);
**DayZ → VRAM oversubscription**, 4.23 GB tracked against a 2 GB budget, and whole-texture eviction
cannot help because `RetainGpuModel` pins every texture of every registered model — it needs
**mip-level residency**.

---

## 4. Diagnosed but not fixed — pick these up directly

- **DayZ: 37 proxy models never load.** `GetShapeName` -> `GetDefaultName(baseName, "data3d\\", ".p3d")`
  at `engine/Poseidon/IO/ParamFileExt.cpp:168` prepends `data3d\` to proxy names that already carry a
  path. Proven present in the PBO at the path minus `data3d\`. Costs the Chernogorsk ferris wheel,
  church crosses, interior furniture. Fix: skip the prefix when the name already contains a separator.
  Not applied — shared engine code on every world's proxy path; smoke-test OFP content.
- **DayZ: 86.6% of placements are the wrong colour.** `dz\plants\*` is 2,572,372 of 2,971,218
  placements. DayZ authors foliage albedo brown/orange and relies on `TreeAdv`'s
  `treeColorization[] = [0.12, 10, 10, 0]` plus a WorldPos-mapped Stage3 map; we have no `TreeAdv`
  colorization and bind the raw albedo. (The "RVMAT `ambient` applied literally" theory was tested and
  **disproved**.) This is likely related to the owner's tree-canopy complaints.
- **`GlobalMapsUVTransform` is ignored by `XobCommand`.** `NetFence_Wire.emat` declares
  `TilingU 24 TilingV 24`; without it cutouts are magnified 24x. **1,208 of 10,312 `.emat` declare it.**
  Directly explains part of "still untextured / looks wrong".
- **`TextureBaker::Write` (`XobCommand.cpp` ~758) overwrites the albedo's alpha with 255** when the
  material names no `OpacityMap`. Only **24 of 309** baked `_MLOD` atlases carry alpha — distant fences
  and probably distant foliage stay solid cards.
- **Nearly every `.emat` resolves no BaseColour, no normal map and no specular map** — the face texture
  is carrying everything. Strong candidate for "assets still untextured" and "building textures wrong".
- **Vehicle track marks still band.** `Tracks.cpp:199` (`TrackStep::Final`) and `Shadow.cpp:81` still
  call `SurfaceSplit` unconditionally — the exact path fixed for roads in `aa24c32`. **Since the owner
  reports Arma 2 roads still wrong, check whether A2 roads reach a different `SurfaceSplit` call site
  than the one guarded.**
- **Everon grows no clutter at all** — 0 surfaces, 0 clutter classes, 0 of 65536 surface cells. The
  task is to make *any* clutter grow, not to swap OFP's for Reforger's.
- **Reforger vegetation does not sway.** 90 of 94 Reforger `.p3d` carry no `map` token, so `MapType` is
  `MapHide` and they get neither sway nor canopy normals.

---

## 5. Operational traps — these cost real time this session

1. **Capture scripts kill each other.** Every harness cleans up by stopping `PoseidonGame` PIDs that
   did not exist at *its* launch, so any run started after another agent's snapshot gets killed
   mid-capture. **Fix: copy the exe to a unique name (`MyProbe.exe`) and match your own cleanup on your
   unique `--log-file` path.**
2. **Shared `dist\` contaminates measurements.** Agents rebuild into it, deploying each other's
   in-flight work. This produced a false "terrain renders white" finding reported independently by two
   agents. Same scene: clean binary 0.0% pure-white pixels, contaminated 65.3%.
3. **Concurrent GPU use inflates every timed region by a common factor** (measured 27.9 ms vs 7.6 ms
   frame). Serialise timing runs; quote the cheapest steady frame, not the median.
4. **The build stamp does not identify a binary.** It is a git commit, and the worktree is dirty — two
   DLLs of different sizes report the same id. Never A/B on it.
5. **The mission hint dialog pauses the world.** The sky is frozen between frames (sky mean |diff| 0.15).
   **Any wind-related verification — cloud drift, vegetation sway — is impossible until the hint is
   dismissed or a mission without it is used.**
6. **`Start-Process -ArgumentList` (PowerShell 5.1) mangles quoting on paths with spaces** and the game
   dies before opening its log — symptom is a missing PNG *and no `.log` at all*. Launch detached via
   `cmd /c "start "tag" /D "<datadir>" "<exe>" ..."`. **Never redirect stdout/stderr** — kills the wgpu
   renderer with os error 232.
7. **Only `GPU frame total` is re-measured every frame** in `--capture-metrics`; one-shot regions
   persist stale values. Never quote them as per-frame costs.
8. **`--screenshot-delay` counts gameplay frames**, not seconds.
9. **OneDrive intermittently locks `status-ledger.yaml`** — `compute_evidence_hash.py` fails with
   `OSError [Errno 22]`. Just retry; it succeeds within a few attempts.

---

## 6. Running it

Binaries deployed 2026-08-15 10:11 to `D:\SteamLibrary\steamapps\common\ARMA Cold War Assault\`
(`ColdWarAssault.exe`, `PoseidonGame.exe`, `wgpu_renderer.dll` — **copy all of them; one alone gives an
instant `Entry Point Not Found`**). Previous binaries backed up at
`C:\Users\mail\AppData\Local\Temp\rf\binbackup-20260815-0315`.

Build from the agent shell (`Build.ps1` dies on a CMake warning under the PowerShell tool):
```bash
export PATH="$PATH:/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64:/d/Program Files/LLVM/bin"
cmake --build build/win-x64-clang-rwdi --target PoseidonGame --parallel 4
```

**Everon** (use `ev_DALL.wrp` — `everon2.wrp` is an early partial export with only 19,644 placements
against 1,216,138, which is why it looks empty):
```
$env:WGR_OBJECT_STREAM_MAX_OBJECTS=200000
.\ColdWarAssault.exe --render=wgpu --window --width=1600 --height=900 --dev `
  --test-mission "dev-missions\devtest-dawn.abel" `
  --test-world "C:\Users\mail\AppData\Local\Temp\rf\world\ev_DALL.wrp" `
  --test-world-freefly 5200 7000 220 45 -8 --test-world-hour 10 `
  --log-file cwr.log --log-level info
```
Sanity check: the log must say `retained 1216138 compact placements`.

**Arma 1 (Sara)** — needs `--addon-root`:
`--test-world "<repo>\packages\a1-compat\world\sara\sara.wrp" --addon-root "<repo>\packages\a1-compat\addons"`,
camera `--test-world-freefly 7345 8555 40 45 -10`. 602,481 placements.

**Arma 2 (Takistan)**: `packages\a2-compat\world\takistan\takistan.wrp`, `--addon-root` pointing at the
nine A2/OA PBO dirs under `inspiration\officialarma`. 438,749 placements.

**Arma 3 (Stratis)**: `packages\a3-compat\world\stratis\stratis.wrp` via `--mod @a3stratis`
(`--addon-root` rejects mod folders). 163,953 placements.

**DayZ (Chernarus)**: `--addon-root "D:\SteamLibrary\steamapps\common\DayZ\Addons"`. 2,971,218 placements.

Capture harness: `C:\Users\mail\AppData\Local\Temp\rf\caplod.ps1` (has `-Width/-Height`, `-Freefly`,
`-LookAt`, `-Budget`, and a retry loop that detects a stale log).

**The installed asset tree is not pristine converter output**: 140 `.paa` under
`...\reforger\everon\textures` had the `FLAG` tagg inserted in place. Originals at
`C:\Users\mail\AppData\Local\Temp\rf\tex-noflag-backup`. Re-running the fixed converter reproduces them.

---

## 7. Ledger

`docs/roadmap/status-ledger.yaml` is authoritative; `compat-c1-capability-matrix.md` is generated and
must never be hand-edited. After any ledger or evidence change, run **in this order**:
```
python scripts/generate_compat_capability_matrix.py
python scripts/compute_evidence_hash.py --all --write
python scripts/validate_compat_c1_ledger.py
```
The validator raises on the *first* bad ticket, so one stale hash hides everything below it. Gate was
green at the end of this session (54 scoped tickets).

---

## 8. Method notes worth keeping

- **Measure denominators, not samples.** "How many of how many" caught three separate bugs this session
  that no spot check would have found.
- **Eliminate hypotheses by ablation, not argument.** Four competing explanations for the texture fault
  were killed with env-gated levers; the CSM road-banding theory was killed the same way.
- Several confident claims this session were **wrong and had to be retracted** — a "too-compressed LOD
  ladder" (native Arma uses the identical 1,2,3,4 ladder), a "capture-resolution artifact", a "terrain
  overexposure defect", a "DPI framebuffer regression", and a **fabricated corpus census that was never
  run**. All are recorded as retracted in the ledger. Expect to overturn things here, and say so
  plainly when you do.
