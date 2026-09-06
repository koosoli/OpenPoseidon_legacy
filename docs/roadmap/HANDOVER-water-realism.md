# Handover — water realism (branch `water-realism`)

Date: 2026-09-06. Plan: [WATER-REALISM-LOW-COST-IMPLEMENTATION-PLAN.md](WATER-REALISM-LOW-COST-IMPLEMENTATION-PLAN.md).
Branch `water-realism` on `safety`, created from `safety/asset-obs-001` at `37a16e16`; upstream had not moved by the end of the session (0 behind). Nothing is merged anywhere. Worktree: `.claude/worktrees/water`, own build dir.

**Installed in the game folder at handover:** `water-realism 7fe5875b 2026-09-06_13:36 deploy-ps1` (exe and DLL from the same build, via `scripts\Deploy.ps1`). The owner is on this build. The other agent's `asset-obs-001 7ece440a` pair was replaced; `7fe5875b` contains it (it is the branch base plus these commits).

## Commits

| commit | package | what |
| --- | --- | --- |
| `b8762b3b` | WRL-002 | One optical model (`water/water_optics.wgsl`) for the surface, the underwater compositor, the froxel volume and the caustics; background composited once; no double fog; `optics_params` lane (params 272 → 288 B). Switch: Water tab > Surface look > "Shared optical model". |
| `a1710b3f` | WRL-002 | Whitecap/foam/spray gated by wind at all three sources (switch: Wave foam > "Whitecaps need wind"); deep swatch default → dark teal (0.010, 0.085, 0.135) per the owner's note. |
| `7fe5875b` | WRL-003 (+ lake parts of 003a/006, 006b hook) | Simulation-owned `WaterBodyRegistry`; body-aware collision surface; per-node `body` lane and a 16-entry body table (params → 800 B); ellipse-clipped per-body draw at its own level; camera containment feeds the compositor; fixtures (env + Water tab). Also the v3 water-look profile rule (v2 `colour` ignored). |
| (docs) | WRL-001..003 | `docs/roadmap/evidence/WRL-00{1,2,3}-*.md` and six PNGs. |

Files touched outside the water-owned set, all minimal hooks: `EngineWgpu.cpp` (profile v3 read/write only), `Collisions.cpp` (one branch: local body surface), `WaterSurfaceQuery.{hpp,cpp}` (scaled overload), `DebugOverlay.cpp` (Water tab controls), `wgpu_renderer.hpp` / `ffi.rs` / `lib.rs` (ABI lanes, underwater view field). `TerrainWgpu.cpp`, `LandLoadEnfusion.cpp`, `Landscape.cpp` untouched. `Cargo.lock` untouched.

## What was measured (all on the installed exe, `perf_water.eden`, VSync pinned off, DLSS on, 3 repeats per arm, sequential arms)

| arm | GPU frame (ms) | Water draw | all water rows |
| --- | --- | --- | --- |
| baseline `7ece440a` | 27.2 / 30.6 / 31.3 | 6.44 / 6.39 / 7.05 | 9.1 / 9.1 / 9.7 |
| WRL-002 `a1710b3f` | 27.6 / 26.6 / 26.3 | 6.18 / 5.87 / 5.90 | 8.6 / 8.4 / 8.5 |
| WRL-003 `7fe5875b` | 26.7 / 27.3 / 27.6 | 5.79 / 6.47 / 6.95 | 8.2 / 8.9 / 9.3 |

Verdict against the section-4 contract: no water regression at any reading; the frame-total arms are within the 5% gate but the arms were not alternated and unrelated rows drifted between them (cloud march, colour pass), so the honest claim is "neutral to slightly cheaper", not a measured win. No new render targets; only the params UBO grew (272 → 800 B). Details, viewpoints, log lines and limitations: the three evidence documents.

Visual (author's reading; owner acceptance pending): whitecap lace from the air gone, calm-sea spray sprites gone, bay shallows clearer; eye-level sea otherwise unchanged (sky-dominated). The elevated lake fixture draws at its own level with the sea in frame, and the compositor engages against the lake's surface from inside it.

## Owner decisions pending

1. **Look:** does the eye-level sea want more sky at grazing angles? That is `reflectionGain` (0.7) and the 0.7 Smith blend in `env_visibility`, not a bug. And whether the darker teal deep swatch is the right direction (it now also sets the absorption hue).
2. **Profiles:** the v3 rule means the owner's four `%APPDATA%\CWR\water-look\*.cfg` files lose their auto-snapshotted swatches (they were the old defaults). Any deliberate colour tuning must be redone once in the Water tab; it will then be saved as v3 and honoured.
3. The lake edge carries the ocean shoreline foam band (bright rim). Weaken it for bodies (first WRL-004 item) or accept.

## What is left (plan order)

- **WRL-002 remainder:** glint/roughness audit at altitude (step 2), rain response capture, a moving-camera sequence, and a native-resolution bench arm. Alternated A/B rounds for a frame-total verdict.
- **WRL-003a:** the inland-only frame fixture (no ocean submitted) is NOT exercised: with the CDLOD ocean always submitted on stock worlds there is no such frame today. Imported-world migration (CalmWater surfaces → bodies) is not started; there is no loader that produces body metadata.
- **WRL-004:** river flow from the body table (fields registered, shader still reads the global lane); per-body interaction/foam pools (fields are camera-anchored and shared, so events leak between bodies at the same XZ); profile tuning with real assets; polygon boundaries and connected mouths.
- **WRL-005:** reflection budget work not started (bodies simply skip planar and keep the sky sample).
- **WRL-006:** finite ray intervals through the side of a lake and dry cavities under a lake are NOT handled: the compositor uses the containing body's plane, so a ray leaving a lake sideways below its level is still fogged. Caustics for bodies use the shared field.
- **WRL-006b:** boats on a lake/river are NOT verified. The collision hook is in; nobody has placed a boat on the fixture. Needs a mission with a small and a large boat on `WGR_WATER_LAKE`, the ocean regression captures, and an observer.
- **WRL-007:** presets/rollout not started.

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
