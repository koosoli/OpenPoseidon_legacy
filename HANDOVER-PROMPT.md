# Handover — what is still open after 2026-08-17

Branch **`feat/texture-streaming`**, worktree
`C:\Users\mail\OneDrive\Documents\GitHub\CWR-CE-paavohuhtalafork-\.claude\worktrees\texstream`.
Push to `safety` (private mirror) after every milestone. HEAD at the time of writing: `cb2736f`.

Deployed binary: **12:02, 2026-08-17**. Reforger game data re-exported **11:34** (rollback tree at
`D:\reforger-p3d-backups\<timestamp>\`, robocopy line in `%TEMP%\rf\reexport-*\`).

---

## Ground rules learned the hard way this session

1. **Never run `.tmp-shadow/capture-arms.ps1` while the owner is playing.** Its cleanup does
   `Get-Process ColdWarAssault | Stop-Process -Force` — it will close his session.
2. **Subagents get killed mid-edit by the usage session limit.** When that happens: save
   `git diff -- <their files>` to `.tmp-shadow/wip-patches/`, stash so the build is clean, and
   respawn with a brief that starts "run `git diff`, read the untracked files, finish it".
   Old agent IDs are NOT resumable after a session rollover.
3. **PowerShell 5.1 + `$ErrorActionPreference='Stop'` turns any native stderr line into a
   terminating error.** Call tools as `& $exe @args 2>&1 | ForEach-Object { "$_" }`.
4. **`--test-world-max-objects N` truncates the world**: it keeps the first N placements in file
   order, and file order is spatial, so whole regions get no objects. Chernarus places 2,971,218.
   It is a triage lever, never a performance setting — streaming already bounds what is resident.
5. **Score the region, and score consecutive frames.** A 30-frame gap measures wind, not flicker;
   an empty-scene control (0.01%) is what makes a grass number mean anything.

---

# OPEN — in the owner's priority order

## 1. Card grass still flickers ~3.5x more than procedural blades

**Measured** (Takistan `6970.40 5700.31 352.57 az 20 el -18`, hour 12, consecutive frames 900/901,
fraction of grass pixels changing by >48/255 — the popping signature, not sway):

| arm | >24 | >48 |
|---|---:|---:|
| empty scene (control) | 0.02% | 0.01% |
| procedural blades | 7.66% | 0.83% |
| cards, before today | 13.05% | 4.77% |
| **cards, shipped now** | **7.72%** | **2.89%** |
| cards with `WGR_GRASS_CARD_A2C=0` | 16.49% | 10.16% |

So: alpha-to-coverage is load-bearing (do not turn it off), density was the biggest lever
(`photoCoverage 0.24`, `photoCoverageAuto=false` now default), and **2.89% vs 0.83% is what is
left**. Evidence `.tmp-shadow/cards{4,5,6,7}/`.

**Next step, cheapest first:** `photoCardScale` is still 1.0 and our card is 2-3x the plate area
of the Arma clutter plant it stands for — try 0.5 and re-score; then the per-slice alpha-edge
report already logged at atlas upload (`Wgpu grass atlas alpha edges ... worst first`) names the
varieties whose masks have the ragged edges, and the owner has said before that a single variety
stopped the flicker. `photoSelfShadow=0` in the Grass tab isolates the CSM grass-on-grass term,
which was never ruled out.

## 2. Reforger buildings: roofs with no walls

Not the sink and not the culling. The walls are **prefab children the exporter never produced** —
`13,449 placements skipped: prefab resolves to no converted model`. Owner pose
`4875.35 6954.80 181.51 az 282 el -15`. This is exporter coverage work in `XobCommand.cpp`
(`xob export` resolves prefabs to models; the children of a prefab are not walked).

## 3. Reforger "buildings render depending on the viewing angle"

Owner pose `1222.42 6003.50 40.35 az 339 el -10.7`. **Not reproduced from a static capture** —
both the default and `WGR_GPU_OCCLUSION=0` arms drew the building fine
(`.tmp-shadow/reforger/cull/`, `.tmp-shadow/reforger3/owner-cull.png`). The normal/winding fix
and today's UV fix may have removed it. **Ask the owner to re-check before investigating.**

## 4. Reforger near trees stuck on a coarse LOD

Registration hypothesis is refuted (618 models log `lod=0`). The LOD governor remains the
suspect: `Scene::AdjustComplexity` drives `_lodInvWidth` over a 16x range from **wall-clock**
frame time, and the GPU is a small share of the Everon frame, so it strips detail chasing time it
cannot recover. `WGR_LOD_GOVERNOR_TARGET=1` measured +36% on Everon but was never shipped as a
default because one world is not enough. **Three arms (Stratis, Chernarus, Sahrani) and it can
ship.** The proper fix is to feed the loop GPU frame time (`Engine::GetWaterGpuTimings` exposes
it) in `SceneDraw.cpp`.

## 5. Streaming: the last stall lever

Today: moving throughput 20.2 -> **28.9 fps**, p95 216 -> **47 ms**, frames >100 ms 391 -> 160
(one binary, `.tmp-movecam/out/everon-vehicle30-texstream{,-off}/`). Still open:

- **160 frames over 100 ms remain.** The split row (`Object stream cold model: ... [reg= mesh=
  sect= tex= ffi= prox=]`) now says exactly where each cold model's time goes — read it before
  choosing the next lever.
- `WGR_OBJECT_STREAM_REGISTER_MS` (a per-frame GPU-registration budget with placeholder draws)
  was deliberately NOT implemented: it means deferring `AddObject`, i.e. an object-lifetime
  change. That is the next lever if `reg` still dominates.
- Worker texture prepare does **not** cover `.emat`/`.rvmat`-resolved maps (nohq/spec/mask) —
  they are not in the IR. `prep=` vs `up=` in the log measures the uncovered share honestly. A
  worker-safe emat text reader would close it.
- **Static frame rate went 39.3 -> 34-35 fps** on Everon. The OFF arm is 34.1, so it is NOT the
  streaming code: it is the corrected data (leaves now sample real 1024^2 textures instead of one
  texel; every model now carries a geometry LOD). Price of correctness, but if it is worth
  clawing back, that is a texture-residency question, not a streamer one.

## 6. Collision — shipped, NOT yet walked

Everon: 516 of 549 models report `passable=0` (`WGR_COLLISION_STATS=1`); the 33 left are
bushes/decals/litter, which do not block in Reforger either. A1/A2/A3/DayZ got mass from the ODOL
readers. **Nobody has actually walked into a wall in any generation.** That is the acceptance
test, and it is the owner's original complaint.

## 7. Arma 2 roads: the near ribbon is patchy

The white/see-through/night-glow half is fixed and verified
(`.tmp-shadow/road-gpu-vb/ridge-prebuild/{default,legacy}.png`). What remains: the near dirt road
is patchy and partly under the terrain, and `WGR_CONFORM_RASTER=1` moved nothing
(`.tmp-shadow/road-gpu-vb/ridge/{default,raster}.png` are identical). So the conform is not the
mechanism for the patchiness — look at the road's own geometry/decal blend instead.

## 8. Black triangular wedges inside Arma 2 buildings

**The owner ranks this last.** Pose `6761.87 6022.92 303.94 az 18.4 el -1.9`. Inside a wedge the
value is exactly `(0,0,0)` — the scene clear colour — while genuinely shadowed wall 5 px away
reads 27-55. Refuted: GPU occlusion, GTAO, sky bake volumes, prepass skinned-VS mismatch,
colour/prepass LOD divergence. **Stop hypothesising: read back the prepass depth and the colour
depth at a wedge pixel** (or draw the colour pass with depth test ALWAYS as an arm) and dump which
record wrote it.

## 9. Sunlit walls blow out — PAUSED at the owner's request

Local overexposure on a lit surface; auto-exposure meters the whole frame so it cannot fix it.
Look at diffuse/specular response and whether the tonemap shoulder rolls off at all at the top.
Related: materials may carry values the shading model treats as absolute radiance
(`forcedDiffuse` folded into `sun_ambient` on the direct path).

## 10. Smaller / unverified

- **Photo grass "mix" slider** reconnected today (0% = one variety) — never seen in a run.
- **Blade shading options** (self-shadow, contrast, hue variation, root shade) ship at
  defaults that reproduce the old look exactly — unverified by eye.
- **Reset-to-defaults buttons** on every dev-panel tab — unverified by eye.
- **A3 tree sway calm floor** (`WGR_TREE_SWAY_CALM_FLOOR=0.5`) — shipped, never seen.
- **Cockpit translucency**: "visual confirmation still inconclusive" from 2026-08-16;
  `WGR_COCKPIT_FORCEALPHA_LEGACY=1` is the A/B, repro `scripts/cockpit-capture.ps1`.
- **Stones transparent from one side** (MAT-047 partial-alpha granite) — untouched.
- **Auto-exposure pumping on camera turns** — never measured; `scripts/farfield-movecam.ps1`
  exists and would score frame-to-frame luminance delta. If it pumps, narrow Min scale first.
- **Moonlight amount** and **iris/eye adaptation** — shipped 2026-08-16, owner has not judged.
- **Underwater system** — disabled by default (`Engine.hpp` `underwaterEffect=false`), owner
  calls it "not good at all", low priority. Turn it on and capture before changing anything;
  `WGR_WATER_DEBUG_UNDERWATER_EXTINCTION` / `..._INSCATTER` isolate the two halves.
- **DayZ performance** ("performance is lacky") — never measured on Chernarus. Use
  `scripts/farfield-movecam.ps1`, and drop `--test-world-max-objects` (see trap 4).
- **Reforger roads** — NOT a bug: Everon's roads are World Editor splines with no mesh in the
  pak corpus. It is a missing feature, not a broken one.

---

# Worlds and commands

| world | `-World` | extra |
|---|---|---|
| Reforger Everon | `%TEMP%\rf\world\ev_DALL.wrp` | — |
| Arma 3 Stratis | `%TEMP%\ff-worlds\stratis\stratis.wrp` | `-Mods a3stratis` |
| DayZ Chernarus | `<repo>\packages\dayz-compat\world\chernarusplus\chernarusplus.wrp` | `-Addons "D:\SteamLibrary\steamapps\common\DayZ\Addons"` |
| Arma 1 Sahrani | `%TEMP%\ff-worlds\sahrani\sara.wrp` | `-Addons "<repo>\packages\a1-compat\addons"` |
| Arma 2 Takistan | `%TEMP%\poseidon-a2oa-takistan-inspect\takistan.wrp` | `-Addons "<repo>\.tmp-perf-modernisation\a2-addons"` |

The Arma 2 addon corpus is the only copy on the machine — read it, never move or delete it.
Everything under `%TEMP%` is deletable by Windows Storage Sense; check the file exists before
blaming a failed load.

Build and deploy (from the worktree):
```bash
export PATH="$PATH:/c/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64:/d/Program Files/LLVM/bin"
cmake --build build/win-x64-clang-rwdi --target PoseidonGame PoseidonTools PoseidonTests -- -j 10
G="/d/SteamLibrary/steamapps/common/ARMA Cold War Assault"
cp build/win-x64-clang-rwdi/apps/cwr/Game/PoseidonGame.exe  "$G/ColdWarAssault.exe"
cp build/win-x64-clang-rwdi/apps/cwr/Game/wgpu_renderer.dll "$G/wgpu_renderer.dll"
```
Reforger re-export (needed after any xob/converter change):
```
.tmp-shadow\run-reforger-reexport.ps1 -PoseidonTools <abs exe> -CopyAllModels
```

---

# What was fixed on 2026-08-16/17, so nobody re-opens it

Walk-through-walls on A1/A2/A3/DayZ (imported models arrived with mass 0, so the engine treated
them as passable); Reforger collision (the `.xob` COLL chunk is real collision geometry, now
converted into a Geometry LOD with convex components); Reforger leaves as solid planes (the `.xob`
vertex streams are pos/normal/**colour**/UV — reading UVs first made every leaf card sample one
texel); Reforger materials opening at all (deploy-root fallback + sidecar `.emat`), rocks swaying
(MLOD branch never set `_mapType`), FLAG tagg on cutout textures; Arma 2 roads white/see-through/
glowing; card grass dark plates (A2/OA clutter plates are authored dark for a material multiplier
this engine never read) and the 10 Hz turbulence term cards were taking from the blade wind field;
grass panel (0.24 coverage, mix slider, blade options, reset buttons); A3 tree sway on calm days;
moving-camera streaming stall 11.9 -> 28.9 fps.
