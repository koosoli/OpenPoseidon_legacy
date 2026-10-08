# Optional ReShade preview

This is an opt-in **single-player Windows x64 Vulkan** experiment. Normal engine
launches default to ReShade off. No system-wide Vulkan layer or registry entry is
installed. ReShade is a separate post-processing runtime, not a replacement for
the engine's materials, GTAO, GI or fog. It cannot add missing map assets.

## Prepare and run

From a Windows development checkout, with Python 3, curl, CMake, Ninja and the
engine's C++ toolchain available:

```powershell
.\scripts\Prepare-ReShade.ps1
.\scripts\Launch-ReShade.ps1 -GameArguments @('--window','--width','1600','--height','900')
```

Preparation downloads the official 6.8.0 full-add-on installer from
[reshade.me](https://reshade.me/), verifies its pinned SHA256, and extracts its
embedded x64 runtime without executing setup. API headers come from official
[crosire/reshade at 18deaa52](https://github.com/crosire/reshade/tree/18deaa52de0c425a78b329e9cb3c497281cd00ec).
It builds our small add-on and places everything under
`%LOCALAPPDATA%/OpenPoseidon/reshade/6.8.0`. No third-party preset/add-on packs are
installed. The source checkout contains only our glue and original comparison
effect; third-party downloads remain outside Git.

The launcher selects Vulkan, adds the layer only to its child process, and
restores the invoking process environment afterward. It holds the repository's
game lock and refuses an already running game. For initial compatibility
isolation DLSS is disabled in this opt-in launch; `-EnableDlss` allows testing it
later. Pass normal launch arguments as the `GameArguments` string array. Do not
use the full-add-on experiment for multiplayer.

Alternatively, after preparing once, enable **Dev Tools → Graphics → Load
optional ReShade on next launch**. This persists `optionalReShade` in
`graphics.cfg`; restart manually to load/unload the runtime. Before renderer
creation the engine verifies the official DLL and manifest hashes and required
cache files, then enables process-local Vulkan layer discovery. A missing or
mismatched cache is reported in the log and panel and keeps the normal renderer.
There is no automatic download, installation or restart. Known multiplayer
startup arguments skip this preview. An explicit `WGR_DLSS` launch value is
preserved; otherwise this experimental path disables DLSS for isolation.

**ReShade effects** switches the actual ReShade runtime on/off live. If the
add-on is absent the panel explains how to opt in;
it does not silently substitute a native effect. **Home** opens ReShade's own
preset controls. Our default comparison effect is a mild color-only sharpening
and contrast adjustment, with a strength slider in that overlay. It needs no
depth buffer and avoids duplicating the engine's ambient occlusion.

**Save ReShade before/after** records two images from ReShade after presentation
effects, named `Native` and `ReShade`, in the runtime's `Screenshots` directory.
Keep the camera still; the capture briefly switches effects off/on and restores
the original state. The engine's ordinary pre-presentation screenshot can omit
injected effects, so it is not sufficient evidence for a comparison.

For unattended validation, the existing localhost JSON harness exposes:

```json
{"cmd":"optional_reshade","action":"state"}
{"cmd":"optional_reshade","action":"off"}
{"cmd":"optional_reshade","action":"on"}
{"cmd":"optional_reshade","action":"capture"}
```

It requires `--dev` and rejects multiplayer. Responses contain `ready`,
`enabled`, `techniques`, `activeTechniques`, `captureCount`, `captureStage`,
`presentCount` and `queued`. Commands queue work for ReShade's presentation
callback; poll `state` for the applied result. Wait for `ready` and at least one
actual enumerated technique before capturing. `captureCount` advances after both
screenshot API calls, not after a file-system success acknowledgement; validate
the actual `Native` and `ReShade` files separately. On/off does not change the
per-technique selection, so `activeTechniques` can remain positive while the
global `enabled` flag is false.

Freshly prepared configurations use the official `OVERLAY/TutorialProgress=4`
(Skip Tutorial) and `ShowScreenshotMessage=0` settings for clean comparisons.
Existing user INI files are preserved; add these settings there if needed. Close
the ReShade overlay and wait at least six seconds after effects finish loading
before capturing, since the official startup splash still lasts five seconds.

## Upgrade the existing cache to licensed scene previews, offline

Use a current integrated engine containing the scene-depth bridge and actual
WGPU target-settings gate. Close the game. The upgrade below uses the existing
official 6.8 runtime/SDK and previously cached pinned GPL DH/CC0 header sources;
it has no download fallback. It builds the current repository add-on in a fresh
stage, checks all fixed shader/preset hashes and defaults to a read-only plan:

```powershell
$offline = 'C:/Users/mail/.codex/worktrees/worldgen-features-20261008/build/reshade-depth-offline-sources'
./scripts/Upgrade-ReShadeScenePreview.ps1 -LocalSources $offline
```

The source directory must contain `AlucardDH/dh-reshade-shaders/LICENSE`,
`AlucardDH/dh-reshade-shaders/Shaders/dh_uber_rt.fx` and
`crosire/reshade-shaders/Shaders/ReShade.fxh`, with their existing pinned hashes.
A missing or different source fails before staging; it never fetches a substitute.
The wrapper prints the fresh stage and complete `UPGRADE-RESULT.json` plan. To
prepare and apply in one explicit command, add `-Apply`. To apply an already
reviewed stage without rebuilding, with the game closed:

```powershell
python scripts/Upgrade-ReShadeSceneCache.py install "$env:LOCALAPPDATA/OpenPoseidon/reshade/6.8.0" '<prepared-stage>' --apply
```

Only the fixed scene-probe/DH source, helper, licence, scene/Off templates,
provenance and our compiled add-on are installed. Original DH source stays in
`Sources`, outside the effect scan; unsupported legacy generic-DH presets are
not added to the normal cache. The official DLL/manifest, ReShade.ini,
OpenPoseidon.ini color strength, custom/session presets and graphics.cfg are
preserved. Conflicting fixed assets refuse the upgrade before destination writes;
use a separate runtime to preserve those custom files. Replacing our previous
add-on/receipt creates `Backups/scene-preview/<id>` with old bytes, config copies
and the plan. An installation failure rolls back completed payload replacements.

This does not enable runtime loading. In developer single-player WGPU, enable
**Load optional ReShade on next launch**, restart, then choose **Scene depth** or
**DH depth debug** in **ReShade preview** before experimental **DH GI**. The live
settings gate requires actual output-sized HDR targets and active 1x/4x MSAA; it
does not change graphics settings. Depth/GI needs explicit selection, and a
remembered engine session preset starts Off on the next launch. An existing
manually selected external/color startup preset retains its existing behavior
so the upgrade does not silently change user tuning or startup configuration.

DH GI also requires an updated bridge and a fresh producer capture explicitly
proving native surface fog is disabled. With active or unverified native fog,
Graphics reports **DH GI paused**, the bridge reports `previewStatus=7` and a
reason, and effects stay off. Depth debug still works. This preserves native
fog; it never changes weather/fog settings. Fog-free GI remains experimental
pending paired image acceptance. The 1000 m working range alone does not make
fogged backbuffer colors valid lighting donors.

The installed harness accepts `-FogAmount` (default `0.25`, matching earlier
stock-Everon receipts). Run these as separate owned launches under the shared
game lock, with the integrated installed commit and an already prepared stage:

```powershell
./scripts/Test-ReShadeDepthInstalled.ps1 -ExpectedCommit <commit> -RuntimeDirectory <stage> -Mode SceneDH -MsaaSamples 4 -UsePreviewControl -FogAmount 0 -MeasureGpu
./scripts/Test-ReShadeDepthInstalled.ps1 -ExpectedCommit <commit> -RuntimeDirectory <stage> -Mode SceneDH -MsaaSamples 4 -UsePreviewControl -FogAmount 0.25 -ExpectFogRefusal
```

The first retains the fixed-camera native/processed beauty pair and true GPU
timing windows; operator image judgement is still required. The second expects
status 7 instead of waiting for status 2, requires a fresh valid bound depth
witness with the producer's fog-disabled flag false, and verifies explicit
native-fog errors for `on`, `capture` and `measure`. It checks disabled effects,
unchanged capture/timing queues, saves a native screenshot, then selects Off and
checks depth release and clean exit. Its receipt sets `fogRefusalPassed` and
`testSucceeded` on success, while `captureSucceeded` and `visualAccepted` stay
false. It does not claim processed beauty or performance acceptance. Expected
refusal cannot be combined with measurement or resize acceptance. Fog values
use invariant decimal formatting in both mission and runtime setup.

The loading checkbox defaults false in `GraphicsConfig` and uses the same
`GamePaths::UserDir()/graphics.cfg` as startup. On normal Windows CWR paths this
is `%APPDATA%/CWR/graphics.cfg`; `POSEIDON_USER_DIR` overrides it for an isolated
profile, and legacy `--oldpaths` uses the game working directory. The upgrade
never edits any of these preference files. Runtime cache location is separately
`%LOCALAPPDATA%/OpenPoseidon/reshade/6.8.0`, unless explicitly overridden.

DH source is GPL-2.0, the common header CC0 and our independent projection helper
MIT; the pinned licence/source receipts are retained. No paid RTGI shader is
installed. See [the depth/RT audit](RESHade-DEPTH-RT-AUDIT-20261008.md) for actual
installed evidence and remaining 4x/quality/GPU timing acceptance. Selecting an
effect is not a claim that all ReShade effects or ray tracing are integrated.

## Compatibility and provenance

Pinned installer SHA256:
`afe4c8f13048306307983b8b3d41d5bf00a86820440b0e57dea10950e1176445`

Pinned extracted ReShade64.dll SHA256:
`0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7`

ReShade core is BSD-3-Clause; the API files are BSD-3-Clause OR MIT. Preparation
retains the upstream licence and writes `PROVENANCE.json`. Our bridge and FX
effect use GPL-3.0-or-later. No RTShade, ReShade-2026 binaries, proprietary shader
packs, or copied game assets are included.

Validated during initial implementation: standalone bridge compilation, exported
control ABI, PowerShell/Python syntax, and a separate non-game Vulkan instance
probe that successfully loaded the explicit ReShade layer and registered our
API20 add-on. **That probe does not establish game presentation compatibility or
visual quality.** Installed-game shader compilation, live toggle, final-frame
before/after captures, resize/fullscreen and frame-time checks remain required
before treating this as accepted support.

Integrated installed acceptance subsequently passed on paired build `64b1df89`:
normal/opt-in launches, live effects, final-frame stock-Everon comparisons,
persisted off/on/missing-cache fallback, actual borderless/windowed resize and
clean exit. See [the integrated receipt](../verification/integrated-earth-material-reshade-20261008.md).
This does not establish depth effects, arbitrary third-party add-ons, DLSS with
ReShade, exclusive fullscreen or complete injected-GPU timing support.

For a crash, collect the engine log, optional runtime `ReShade.log`, GPU/driver,
engine exe/DLL versions, preset/add-ons and exact launch command. First compare
a normal launch with an opt-in launch containing only our color effect. Test
depth effects only after validating reversed-Z depth selection, MSAA and render
scale. Uninstalling the experiment means closing the game and removing its
dedicated optional cache directory. Disable **Load optional ReShade on next
launch** to restore ordinary startup; no global layer needs removing.
