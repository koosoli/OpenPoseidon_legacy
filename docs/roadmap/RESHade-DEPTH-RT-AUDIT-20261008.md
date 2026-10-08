# ReShade depth and screen-space tracing audit, 2026-10-08

**The existing integration does not contain RTGI or all ReShade effects.** It
loads the official optional 6.8.0 Vulkan runtime, exposes live global effects
controls and final-presentation before/after captures, and supplies one original
color-only sharpen/contrast shader (`OpenPoseidonLook.fx`). The existing installed
color-only acceptance remains useful but establishes neither depth access nor
screen-space GI. This audit inspected the current parent worktree's
`scripts/Prepare-ReShade.py`, `tools/reshade-bridge/ReShadeBridge.cpp`,
`engine/Poseidon/Dev/Debug/ReShadeOptional.cpp` and renderer depth conventions.

## What the linked guide actually describes

The [requested forum guide](https://reshade.me/forum/shader-discussion/5450-guide-to-ray-tracing-with-reshade)
describes Marty McFly's depth-dependent **screen-space** ray tracing. Its old
Patreon tiers, beta status, predicted future availability and parameter recipes
are historical information, not current download terms or engine-specific
calibration. Its useful constraints are that valid scene depth is needed,
off-screen information is missing, and fog/water/halos/temporal artifacts require
care. Do not repeat its blanket superiority claims as verified measurements.

The current [author's RTGI page](https://www.martysmods.com/rtgi/) still directs
downloads to Patreon. No user-provided licensed RTGI archive was identified or
installed. No purchase, third-party mirror, paywall bypass or redistribution is
part of this work. [qUINT](https://github.com/martymcmodding/qUINT) is a separate
collection (MXAO/ADOF/Lightroom/Bloom/SSR/sharpen/deband); its README says all rights
reserved. Public availability alone does not authorize bundling its shaders.

## Actual depth prerequisites

ReShade's [pinned add-on manager](https://github.com/crosire/reshade/blob/18deaa52de0c425a78b329e9cb3c497281cd00ec/source/addon_manager.cpp)
loads its built-in **Generic Depth** add-on unless disabled. Our bridge supplies
no custom engine-depth handoff. Generic Depth tracks candidate depth resources
and [binds the selected view to the DEPTH semantic](https://github.com/crosire/reshade/blob/18deaa52de0c425a78b329e9cb3c497281cd00ec/examples/09-depth/generic_depth_addon.cpp).
That source also updates `bufready_depth`, which the new probe uses. Therefore
the optional official runtime may already expose valid WGPU Vulkan depth, but
the existing color-effect receipts do not prove that it does.

The engine uses reversed-Z, cleared to zero, with an infinite-far projection:
main depth is `near / viewZ`. This is established in
`engine/WgpuRenderer/rust/src/gfx3d/mod.rs` and its depth reconstruction tests.
Shadow maps also exist and are unsuitable substitutes for scene depth. Scene
resource selection, resolution/offset, vertical orientation, pre-clear
preservation, MSAA and render scaling must all be checked. Reversed-Z alone is
insufficient. The [official common header](https://github.com/crosire/reshade-shaders/blob/fd0022170615ce0d8162d219bff07232fa6dd84f/Shaders/ReShade.fxh)
has orientation/reversal/scale and finite far-plane linearization controls;
its default far-plane value is not the engine camera's exact metric projection.

## Bounded addition

`scripts/Prepare-ReShadeDepth.py` stages an **original GPL depth availability
probe**, separate off/probe presets, and optionally the author's freely licensed
[DH_UBER_RT](https://github.com/AlucardDH/dh-reshade-shaders/tree/0783b8fde77df48412d79070be7de4bc3743b898).
The author describes that shader as screen-space GI/AO/reflections. Its pinned
[LICENSE](https://github.com/AlucardDH/dh-reshade-shaders/blob/0783b8fde77df48412d79070be7de4bc3743b898/LICENSE)
is GPL-2.0. It remains a separately downloaded FX source, not linked/copied into
the engine. The preparer retains its license and source without modification,
plus the official `ReShade.fxh` with CC0-1.0 SPDX. It writes URLs, exact SHA256s
and byte sizes to `DEPTH-PROVENANCE.json`. No paid Marty shader is included.

The probe samples raw DEPTH directly and expands its range for visibility. Its
top strip is red when Generic Depth reports no bound resource and green when a
resource is bound. **Green is not proof of correct scene depth.** Check aligned
geometry and changing depth in the image. The probe deliberately does not claim
metric linearization, AO, GI or ray tracing.

The optional DH preview has SSR and source-ambient removal off, moderate AO/GI,
one-frame accumulation and a one-third ray-generation scale. It is a starting
point for a **stationary** comparison, not an accepted quality preset. Its motion
texture is not supplied by another effect, so this does not establish temporal
motion support. The full shader's GPU cost can be substantial even with SSR off.
Its DX9-only external noise image is not needed by this Vulkan experiment.

Preparation never changes `ReShade.ini`, the selected preset, `OpenPoseidon.ini`,
the engine preference or any global Vulkan registration. Existing differing
files cause a refusal, preserving user tuning. Only explicit preset selection
enables a new technique. `--probe-only` uses no downloads and does not stage the
large DH effect, keeping the first depth test small. A normal game launch remains
off unless the user has independently opted into the existing ReShade feature.

## Exact parent-owned validation/capture plan

The parent session owns game launches/deployment and the shared game lock. No
game was launched and no shared runtime was changed for this addition.

1. Read `DEPLOYED-FROM.txt`, verify the paired binaries, and hold the game lock.
   Use an **isolated copy** of the already verified optional runtime for this
   experiment. Preserve the owner's active cache, preset and preferences.
2. Stage only the probe first:
   `python scripts/Prepare-ReShadeDepth.py <isolated-runtime> --probe-only`.
   In that isolated runtime's copied `ReShade.ini`, explicitly select
   `PresetPath=.\Presets\OpenPoseidon-Depth-Probe.ini`. Use the existing launcher
   with `-RuntimeDirectory <isolated-runtime>` so layer scope stays process local.
3. Reuse the exact stock Everon scene and pose from
   `scripts/Test-OptionalReShade.ps1`: 1600x900, view distance 3000, freefly
   `8131.43 4086.37 191.04 229.1 -11.3`, 1985-06-21 16:00, fog/overcast .25,
   rain zero. Keep DLSS off and exposure fixed at .5. Wait for compilation and
   at least six seconds after the startup overlay finishes.
4. Check `ReShade.log` for successful probe compilation and no runtime errors.
   Capture with the existing final-presentation `optional_reshade/capture`
   command, polling `captureCount`/`captureStage` and checking the actual files.
   This gives a labeled native-versus-depth-diagnostic pair, not a beauty pair.
   The depth image should show aligned building/terrain silhouettes and farther
   surfaces brighter than nearby ones; sky should remain at the far end.
5. If red/uniform/wrong: inspect Generic Depth in ReShade's Add-ons tab. Confirm
   it is enabled, inspect candidate resource dimensions/draw counts, and try
   only the matching scene resource plus depth-copy-before-clears where needed.
   Record exact choices and a second probe capture. If no correct resource is
   available, stop the bounded test and report that blocker; do not present
   color grading or a black effect as working tracing. A dedicated engine-depth
   handoff is a separate renderer task.
6. Only after correct depth is established, run the preparer without
   `--probe-only` against the same isolated runtime. It downloads exactly three
   pinned source/license files, total **132580 bytes**. Select
   `OpenPoseidon-DH-Depth-Debug.ini` to check DH's own depth reconstruction;
   calibrate depth settings to the actual projection before selecting
   `OpenPoseidon-DH-RTGI-Preview.ini`. Default reversal is 1 and upside-down is
   0; these are test starting points, not a visual acceptance claim.
7. For the new GI/AO comparison, use the same locked stationary camera, settings
   and native effect state in both images. For an isolated external AO/GI trial,
   set `WGR_GTAO=0` and `WGR_GI=0` **in both arms**, then disclose that choice.
   Capture native versus DH using ReShade's final-frame capture. Recheck the
   selected technique and log, because the bridge's count includes all compiled
   techniques and does not identify which one produced the pixels.
8. Inspect roofs/wall-ground contacts, thin foliage, sky, weapon/HUD, fog and
   water for halos/leaks. Then make a small camera movement and return to the
   recorded pose to test disocclusion/flicker. Report motion as unsupported if
   the missing motion provider prevents a clean result. Measure total frame time
   externally: engine GPU timestamps exclude injected work. A passing still
   does not establish resize, upscaling, multiplayer or all-effect compatibility.
9. Restore the original selected preset/preferences; leave ordinary runtime
   loading off unless the user chose otherwise. Keep screenshots, logs, preset,
   provenance, GPU/driver and stamped build together. Label **depth diagnostic**,
   **DH screen-space GI/AO**, and **existing color look** separately.

## Verification completed here

Python syntax compilation and three offline tests pass: probe-only preparation
makes no network calls and preserves active configuration; corrupt source hashes
write no shader; conflicting user shader files are preserved before other writes.
Full source preparation was run in an isolated build directory, verified all
three pinned hashes, and subsequently repeated from local cached sources. The
final staged assets are in `build/reshade-depth-final` in the feature agent's
worktree. **FX compilation, actual depth availability, new before/after images
and total frame cost remain unverified until the parent runs the capture plan.**
