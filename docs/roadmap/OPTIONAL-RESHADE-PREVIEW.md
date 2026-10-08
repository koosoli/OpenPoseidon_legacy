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

For a crash, collect the engine log, optional runtime `ReShade.log`, GPU/driver,
engine exe/DLL versions, preset/add-ons and exact launch command. First compare
a normal launch with an opt-in launch containing only our color effect. Test
depth effects only after validating reversed-Z depth selection, MSAA and render
scale. Uninstalling the experiment means closing the game and removing its
dedicated optional cache directory. Disable **Load optional ReShade on next
launch** to restore ordinary startup; no global layer needs removing.
