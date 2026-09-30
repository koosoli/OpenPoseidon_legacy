# Original stock forest installed capture fixture

Status: **RUNNER CANDIDATE; installed execution and visual acceptance pending.** This fixture accompanies the source forest family policies, not a claim that all forests or every OFP map have accepted snow. The owner's white billboard regression supersedes the earlier individual-tree visual acceptance.

Run `scripts/Test-ForestSnowRuntime.ps1` through `scripts/with-game-lock.sh`, supplying `-ExpectedCommit` for the installed `codex/streaming-residency` pair. `-World All` runs four serial processes (Everon/Eden and Nogova/Noe, tree snow ON/OFF). `-World Eden` or `Noe` narrows the campaign. `-SelfTest` exercises source-proof, material-trace, pause-state, finite camera and null-safe environment helpers without launching a game or using the GPU. The runtime script never builds or deploys.

Example from Git Bash, using the bundled PowerShell path already configured for this machine:

```bash
LOCK_OWNER="forest snow runtime" scripts/with-game-lock.sh \
  /c/Users/mail/.cache/codex-runtimes/codex-primary-runtime/dependencies/native/powershell/pwsh.exe \
  -NoProfile -File scripts/Test-ForestSnowRuntime.ps1 -World All -ExpectedCommit 12345678
```

Replace the example commit with the reviewed installed stamp. Both the EXE and renderer DLL hashes, sizes, timestamps and the exact stamp must remain identical before/after every arm. The stamp is read before either binary. Runtime results remain `captured-not-visually-accepted`; lifecycle `ok` only means a normal owned exit with shutdown marker and no detected validation failure. Failed or forcibly cleaned-up arms retain a failed campaign record and never receive accepted lifecycle metadata.

## Actual original-map targets

Offline `PoseidonTools terrain objects` reads installed source world placements; its printed positions are rounded to decimetres. Runtime read-only `stream_identity_probe` binds the actual original world ID, exact model, finite twelve-value owner frame, live position/radius, intact state, visual residency and nonzero normal visual buffers. No spawning, retaining, pinning, fabricated canopy cap or synthetic renderer owner is involved.

| World | Actual original owner | Rounded source position X/Y/Z | WRP SHA256 |
| --- | --- | --- | --- |
| Eden/Everon | ID4043, `data3d\les trojuhelnik pruchozi.p3d` | 5025 / 57.5 / 4225 | `5A2550B27612FC4C8CCDEC55CE2555C1ED23114D46F8B687CE5FB8B273515A79` |
| Noe/Nogova | ID173643, `o\tree\les_nw_jehl_t1.p3d` | 1626.3 / 132.9 / 10077 | `9B27B11661D3CB30F9F0749A9A4616BFC58D723C4750666D1E674DD4213598D2` |

Eden source is `Worlds/eden.wrp`; Noe source was extracted from the installed original `O.pbo`. The nearest audited Eden merged forest is 137.49m from Oliver's camera X5018.51/Z4087.66. The exact owner-scene pose `5018.51 4087.66 117.58 220.8 -86` is also captured on Eden. That extra scene is not promised to centre ID4043.

The runner reuses `perf_field.eden` and `perf_sand.noe` only to boot the respective original worlds. The sand walking fixture is not executed. Both mission files, private graphics.cfg and the script are provenance inputs. Private profiles use `Write-BenchmarkGraphics.ps1`, 1280x720, MSAA4, DLSS off, brightness1 and fpsCap60. Native world overrides are removed. Actual liquid rain, particles and fog must be zero; wind is zero; cloud coverage, temporal processing, adaptive exposure, terrain jitter, grass and puddles are disabled. Date is 1985 June21 16:00, simulation time100 is frozen, exposure1. Fixed 18cm snow is explicitly deposited after scene readiness, without falling flakes or a snowline.

## Captures and proof limits

Live owner origin/radius and read-only physical terrain heights derive cameras at horizontal distances60/200/500m and a steep top view. The aim is an explicit approximation above the actual origin, not a claimed loaded crown bounding box. Each PNG retains its requested pose, owner frame, actual snow/weather state, hash and loaded traces. Near camera return and snow disable/restore captures preserve the same frame, exact paused time and stored chunk count. Disabled controls cover near/far/top; restored controls cover near/top. ON/OFF arms use the same binary, owner and camera poses. Ground snow remains available in the tree-OFF arm.

`FOREST_SNOW_OWNER` establishes model-level retained ForestPlain/Forest authority. `FOREST_SNOW_ATLAS` establishes actual loaded slot/resolution/texture/category/alphaRef registration. Both are required for the bound model, along with a ready/submitted physical weather-map trace and the actual 18cm raised-surface snow upload. Registration rows **do not identify the selected LOD in a given screenshot**, and model-level owner rows do not carry a world owner ID. The live world identity and renderer registration are complementary evidence, not a invented joined draw trace. `WGR_TREE_SNOW` is recorded as inherited process configuration; this source switch has no separate startup renderer echo. Bounded early map traces do not prove ongoing per-camera fragment exposure, full caster completeness or all-world dynamic shelter.

Root must inspect the images: top canopies should read snowy while keeping photographed foliage relief; near crowns should retain branch/leaf texture and alpha holes; trunks/ground tiles must not become uniform white slabs; the 200/500m forest silhouette must avoid solid white wedges. Compare ON/OFF, disabled and restored images and verify that the intended target is visible. Camera-return and cross-process image differences are descriptive until measured; the script does not invent semantic segmentation, exact repeat pixels, selected-LOD proof, performance acceptance or all-map completeness. If a required owner/material/map/state gate fails, preserve the failed evidence and repair the fixture or source; do not waive it.
