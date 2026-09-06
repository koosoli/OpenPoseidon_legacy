# Handover — water realism (branch `water-realism`)

Date: 2026-09-06. Plan: [WATER-REALISM-LOW-COST-IMPLEMENTATION-PLAN.md](WATER-REALISM-LOW-COST-IMPLEMENTATION-PLAN.md).
Branch `water-realism` on `safety`, created from `safety/asset-obs-001` at `37a16e16`; upstream had not moved by the end of the session (0 behind). Nothing is merged anywhere. Worktree: `.claude/worktrees/water`, own build dir.

**Installed in the game folder at handover:** `water-realism 54749c9c 2026-09-06_16:59 deploy-ps1` (exe and DLL from the same build, via `scripts\Deploy.ps1`). The owner is on this build. The other agent's `asset-obs-001 7ece440a` pair was replaced; `7fe5875b` contains it (it is the branch base plus these commits).

## Commits

| commit | package | what |
| --- | --- | --- |
| `b8762b3b` | WRL-002 | One optical model (`water/water_optics.wgsl`) for the surface, the underwater compositor, the froxel volume and the caustics; background composited once; no double fog; `optics_params` lane (params 272 → 288 B). Switch: Water tab > Surface look > "Shared optical model". |
| `a1710b3f` | WRL-002 | Whitecap/foam/spray gated by wind at all three sources (switch: Wave foam > "Whitecaps need wind"); deep swatch default → dark teal (0.010, 0.085, 0.135) per the owner's note. |
| `7fe5875b` | WRL-003 (+ lake parts of 003a/006, 006b hook) | Simulation-owned `WaterBodyRegistry`; body-aware collision surface; per-node `body` lane and a 16-entry body table (params → 800 B); ellipse-clipped per-body draw at its own level; camera containment feeds the compositor; fixtures (env + Water tab). Also the v3 water-look profile rule (v2 `colour` ignored). |
| `331b0f0f` | owner defaults + WRL-004 start | waveScale 0.65 and specPower 140 as defaults (v2 profile values equal to the old defaults are dropped so they reach the install); a third body-table lane carries per-body flow (params 800 → 1056 B); bodies get 15% of the shoreline foam band and no swash, which removed the lake's bright rim; river fixture flows downhill. |
| `2417669d` | owner default + WRL-006 step 7 | Underwater effect ON by default (profiles v4; an older profile's on/off flag is ignored, its tuning kept). The compositor clips each ray's wet path at the camera body's ellipse exit, so a ray leaving a lake sideways below its surface is not fogged past the bank. Bench with the effect on and a dry camera: the underwater rows read -1 (the pass does not run dry), water rows unchanged. |
| `cc44a5a8` | WRL-004/006 + owner request | Objects falling into water splash (one hook in `Entity::Simulate`, `WaterEntrySplash.cpp`); river flow eases toward the bank; the in-scatter volume is clipped at the body side like the compositor. |
| `22a874fa` | WRL-006b/007 fixtures | `tests/perf/missions/lake_boat.eden` (two `BoatW` + a barrel dropped below the lake surface), Water tab "Low"/"Balanced" presets, `WGR_WATER_LAKE_GRID=n` stress fixture. |
| `f8477c0b` | WRL-007 | Body nodes distance-culled past the last CDLOD range; boat class fixed. |
| `5c347a8b`, `e144e87b`, `54749c9c` | WRL-004 rivers | Oriented reach (rotation), two-phase flow advection of the surface detail, flow-aligned foam streaks, mouth fade + below-sea discard, and bed-following surfaces (centreline terrain + depth, CPU and GPU from the same heightfield). Fixture that renders on Everon: `WGR_WATER_RIVER=7250,6290,45,9,37,0,-0.27,0.05,2,1.2,90,0.6` (fields: x,z,rx,rz,level,gradX,gradZ,wave,profile,speed,heading°,bedDepth); picture `evidence/WRL-004-54749c9c-river.png`. |
| (docs) | WRL-001..007 | `docs/roadmap/evidence/WRL-00{1,2,3}-*.md` and six PNGs. |

Files touched outside the water-owned set, all minimal hooks: `EngineWgpu.cpp` (profile v3 read/write only), `Collisions.cpp` (one branch: local body surface), `WaterSurfaceQuery.{hpp,cpp}` (scaled overload), `DebugOverlay.cpp` (Water tab controls), `wgpu_renderer.hpp` / `ffi.rs` / `lib.rs` (ABI lanes, underwater view field). `TerrainWgpu.cpp`, `LandLoadEnfusion.cpp`, `Landscape.cpp` untouched. `Cargo.lock` untouched.

## What was measured (all on the installed exe, `perf_water.eden`, VSync pinned off, DLSS on, 3 repeats per arm, sequential arms)

| arm | GPU frame (ms) | Water draw | all water rows |
| --- | --- | --- | --- |
| baseline `7ece440a` | 27.2 / 30.6 / 31.3 | 6.44 / 6.39 / 7.05 | 9.1 / 9.1 / 9.7 |
| WRL-002 `a1710b3f` | 27.6 / 26.6 / 26.3 | 6.18 / 5.87 / 5.90 | 8.6 / 8.4 / 8.5 |
| WRL-003 `7fe5875b` | 26.7 / 27.3 / 27.6 | 5.79 / 6.47 / 6.95 | 8.2 / 8.9 / 9.3 |
| `331b0f0f` with the owner's live Everon profile (amplitude 1.0, scale 0.65, specular 139) | 29.7 / 29.5 / 31.7 | 8.04 / 8.28 / 7.42 | — |

The last row is not a code regression: between deploys the owner tuned the Water tab live (the Everon profile was rewritten as v3 at 14:00 with amplitude 1.0 where it had 0.42), and a taller, shorter sea costs more water draw. It is the owner's setting; recorded so nobody attributes it to the branch.

Verdict against the section-4 contract: no water regression at any reading; the frame-total arms are within the 5% gate but the arms were not alternated and unrelated rows drifted between them (cloud march, colour pass), so the honest claim is "neutral to slightly cheaper", not a measured win. No new render targets; only the params UBO grew (272 → 800 B). Details, viewpoints, log lines and limitations: the three evidence documents.

Visual (author's reading; owner acceptance pending): whitecap lace from the air gone, calm-sea spray sprites gone, bay shallows clearer; eye-level sea otherwise unchanged (sky-dominated). The elevated lake fixture draws at its own level with the sea in frame, and the compositor engages against the lake's surface from inside it.

## Owner decisions pending

1. **Look:** does the eye-level sea want more sky at grazing angles? That is `reflectionGain` (0.7) and the 0.7 Smith blend in `env_visibility`, not a bug. And whether the darker teal deep swatch is the right direction (it now also sets the absorption hue).
2. **Profiles:** the v3 rule means the owner's four `%APPDATA%\CWR\water-look\*.cfg` files lose their auto-snapshotted swatches (they were the old defaults). Any deliberate colour tuning must be redone once in the Water tab; it will then be saved as v3 and honoured.
3. (done in `331b0f0f`: bodies now get 15% surf and no swash; the rim is gone.)

## Owner's visual pass (owner instruction 16:00: no more visual testing by the agent)

Fixtures, poses and the exact "what to look for" list are in [WRL-004-007-rollout-20260906.md](evidence/WRL-004-007-rollout-20260906.md): the lake, the boats mission, the underwater and side-exit poses, the splash counter, and the river that still needs a valley pose.

## What is left (plan order)

- **WRL-002 remainder:** glint/roughness audit at altitude (step 2), rain response capture, a moving-camera sequence, and a native-resolution bench arm. Alternated A/B rounds for a frame-total verdict.
- **WRL-003a:** the inland-only frame fixture (no ocean submitted) is NOT exercised: with the CDLOD ocean always submitted on stock worlds there is no such frame today. Imported-world migration (CalmWater surfaces → bodies) is not started; there is no loader that produces body metadata.
- **WRL-004:** rivers done as oriented, bed-following reaches with two-phase advection, streaks, bank slowdown and a sea mouth; still missing: obstacle turbulence, per-body interaction pools, profile tuning with real assets, polygon (multi-segment) rivers and river-to-river connections.
- **WRL-005:** reflection budget work not started (bodies simply skip planar and keep the sky sample).
- **WRL-006:** side exits clipped for extinction and in-scatter (done); camera-versus-player is inherent (camera decides submersion). Dry cavities under a lake and per-body caustics are NOT handled.
- **WRL-006b:** the collision hook and the `lake_boat.eden` mission are in; nobody has WATCHED a boat on the lake yet (owner's pass). Planing/turn/grounding/sinking checks, multi-boat CPU cost and the headless path are undone.
- **WRL-007:** Low/Balanced presets, stress fixture and distance cull done; lifecycle stress, a true native-resolution arm (both bench arms reported DLSS inactive at 2305x965, so the comparison is still owed) and the ledger entry are not.

## How to continue

```powershell
cd .claude\worktrees\water
git pull --rebase safety asset-obs-001
cargo test -p wgpu_renderer --profile rwdi -- water underwater shaders ffi planar   # 68 pass
cmake --build build\win-x64-clang-rwdi --target PoseidonGame                        # from Bash with the SDK mt.exe on PATH, see memory
build\win-x64-clang-rwdi\tests\unit\engine\Poseidon\PoseidonTests.exe "[Water]"    # 7 pass
scripts\Deploy.ps1                                                                  # only when no OpenPoseidon.exe runs; read DEPLOYED-FROM.txt first
```
Capture recipes (freefly poses, the 14 s sea-shot rule, the lake fixture env, the scratch user dir for the underwater effect) are in the evidence documents; the scripts lived in the session scratchpad and are reproduced there in full command form.
