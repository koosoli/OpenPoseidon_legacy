# Integrated Earth, materials and optional ReShade, 8 October 2026

Private development branch: `codex/whole-world-horizon-20261008`. Changes are
pushed to the private safety mirror only. No authorization to publish these
changes to the cleaned public release repository has been inferred.

## Earth rendering

Installed paired build `86be544a` passed
`build/earth-locations/20261008-033016/result.json`, including the optional
cached building slice. The same process visited Luxembourg, Alps, Grand Canyon,
the dateline, and returned to Alps. Invalid locations preserved camera/origin;
rapid changes rejected superseded patches. All expected cameras and geographic
revisions matched renderer receipts, and logs had no renderer/device failures.

Luxembourg at 49.61,6.13 loaded 1995 buildings, 46281 triangles, skipped 5 and
estimated 1979 heights. Grounding uses the actual resident elevation grid, with
foundations extending through slopes. The inspected screenshot
`luxembourg-buildings.png` shows neutral footprint extrusions on the terrain.
Moving to an uncached region published an empty mesh rather than carrying the
old town across the globe. Data stay outside Git; the Earth tab attributes OSM.

This is a bounded local-cell presentation prototype. There are no worldwide
buildings/roads/vegetation, interiors, collision, streamed simulation, global
editor placement or seamless globe circumnavigation. NASA overview imagery is
coarse at ground level. These are substantial gaps from VBS4-style whole-world
simulation; this checkpoint must not be described as parity.

The earlier `dbeb9d36` installed control run, with a scripts-only expectation
fix, passed W/S approach/retreat and near/horizon/orbit transitions:
`build/earth-streaming/20261008-023809/result.json`.

## Native Reforger material correctness

The translated `MatPBR2Layers` mask now honors its authored `UVSrcMaskMap`
before falling back to global `UVSrc`. The installed original control-tower
material uses UV set2; the old adapter selected UV0. This correctness fix is
active in the ordinary native import path.

Authored Basic `RoughnessScale` and `MetalnessScale` now survive the matched
C++/GPU material ABI and affect the guarded native PBR shader route. Zero is a
valid authored scale; nonfinite/out-of-range values receive explicit fallback
and bounds. Unproven multi-layer scale semantics have not been guessed.
`WGR_REFORGER_PBR_PREVIEW=1` enables the existing optional native route and its
provenance prerequisite. The preview remains off by default.

Owned synthetic native Basic fixtures passed eligibility and fixed-camera A/B
on installed `292e86fd`, recorded at
`build/native-pbr-basic/20261008-031747/result.json`. Synthetic textures have
owned resource GUIDs; model paths respect the legacy path limit. This is a
channel/scalar contract test, not a Reforger reference-renderer comparison.

Actual native Reforger Village A/B on the same paired source used 1920x1080,
4xMSAA, no DLSS, fixed 16h/exposure0.5, one 60s warmup per arm. Both registered
20683 instances and passed capture lifecycle/settled-preparation checks:
`build/native-reforger-perf/Village-pbr-{off,on}-20261008/run-01.*`.
GPU frame total was 51.58ms off and 50.77ms on. One pair cannot establish a speedup;
no substantial extra cost was demonstrated. A real layered entry material
became eligible, as did Basic `ShopModern_E_01_WindowFrame` with authored scales
0.772/0.772. Roof/fence responses differ visibly, but this toggles the whole
preview and includes differing shadows. It does not isolate the new Basic
scales, establish corpus-wide visual parity or justify enabling globally.

## Actual optional ReShade

Installed `86be544a` passed normal/native startup and process-local injected
Vulkan startup, effect compilation, live off/on, actual final-frame before/after
saves and clean exit:
`build/reshade-acceptance/20261008-033310/result.json`.
Both comparison images were made on stock Everon, using the normal `.eden`
mission loader; the test rejects importer fallback/critical world-load errors.
ReShade's own post-effect capture is used, since engine screenshots precede it.
Tutorial/save notifications were disabled through official runtime INI options.

The preset is an original mild sharpening/contrast effect, without depth-buffer
or ray-tracing claims. Normal startup defaults off. Graphics exposes a persisted
next-launch loading preference and a separate live effect toggle. The verified
official runtime is an optional local cache, not a system-wide layer install.

Engine GPU timestamps exclude injected ReShade work; the stored CPU/engine
samples do not establish total ReShade GPU cost.

## Final paired checkpoint: `64b1df89`

Built and deployed the matching EXE/DLL at 03:51 local time, then passed:

- `build/reshade-preference/20261008-035140-66608/result.json`: saved off starts
  without ReShade; saved on loads the verified cache and real effect; saved on
  with missing cache starts normally with a clear warning. Live off/on passes.
  Actual Alt+Enter goes from 1280x720 to 3441x1440 borderless and restores
  1280x720, with ReShade presentation resuming. All three arms exit zero.
- `build/earth-locations/20261008-035220/result.json`: the 3x3 cached building
  window retains all 1995 buildings/46281 triangles when moving from cell
  2480,306 into uncached center cell 2480,307. The same relocation, camera,
  stale-request and invalid-input checks pass. No extra geographic data was
  downloaded for the neighboring-cell change.
- `build/reshade-acceptance/20261008-035236/result.json`: clean stock-Everon
  final-frame comparison, live effect state, normal/opt-in launches and exits.
  Stable convenience copies: `build/reshade-before.png` and
  `build/reshade-after.png`.
- CPU Earth building checks pass convex/concave/slope geometry, 3x3 retention,
  cross-cell ID deduplication, one shared 200k-triangle cap, cancellation and
  geographic revision rejection. Earth coordinate/adaptive-detail checks pass.

The display test exposed a pre-existing WGPU dummy `SetWindowMode` no-op. WGPU
now performs the actual SDL transition and forwards real pixel extents through
its existing resize path; redundant same-size notifications do not recreate
the swapchain. Exclusive-mode selection uses the existing placement resolver,
but only borderless/windowed switching was runtime accepted on this monitor.

## Local imagery and final installed source: `27b7bf23`

Built and deployed the matching EXE/DLL at 04:20 local time. The Earth tab now
offers Globe and Luxembourg buttons alongside editable geographic coordinates.
The optional `-LocalSurface` launcher switch adds exactly one prepared ACT CC0
Luxembourg orthophoto; `-CachedBuildings` enables the previously tested OSM
extrusions. Sources remain external local caches, with no runtime imagery HTTP.

Installed acceptance:

- `build/earth-locations/20261008-042108/result.json`: NASA baseline, cached
  buildings, adjacent cell, geographic relocation, invalid input and latest
  request checks pass.
- `build/earth-locations/20261008-042202/result.json`: same build/camera with
  the local atlas. Actual Luxembourg imagery publishes at generation4/revision2,
  then generation5/revision2 after the near-window move. Buildings remain.
  Alps, Grand Canyon and dateline publish NASA fallback; no stale local image
  follows the geographic relocation. Clean renderer logs and exit.
- `build/earth-streaming/20261008-042509/result.json`: optional local-surface
  and building workers enabled, rolling near windows, coarse horizons, orbit
  and return all pass. Actual two-second SDL W/S input moves from 12,000,000m
  to 10,860,824m and back to 12,002,561m; the view remains stable.
- `build/reshade-acceptance/20261008-042339/result.json`: normal and actual
  process-local ReShade launch, live off/on, final-frame capture pair, stock
  Everon load and clean exit all pass on the final installed source.

Inspected `luxembourg-buildings.png` baseline/local pair has identical camera
and buildings. The local arm reveals roads, forest and city imagery, with a
feathered boundary into the deliberately coarse global overview. White buildings
remain simple untextured footprint extrusions; most heights are estimates.
Stable local copies: `build/earth-surface-before.png`,
`build/earth-surface-after.png`, `build/reshade-before.png`,
`build/reshade-after.png`. ReShade's change is intentionally subtle sharpening
and contrast; it does not replace the native material fixes.

One local-overview GPU sample was 5.27ms baseline and 5.18ms local at 1280x720,
4xMSAA/no DLSS. These nonblocking samples are not a benchmark or speedup claim.
Both startup-containing CPU windows also include roughly 2.3-second land-phase
maxima, so they cannot establish stutter-free startup or streaming. The new
atlas adds one draw and about 21.33MiB GPU, without new triangles or fullscreen
passes. Worker memory/source-validation bounds and restart-after-preparation
limits are documented in `docs/earth-local-surface-prototype.md`.

The useful bounded slice is complete and tested. VBS4 parity is **not achieved**:
global detailed imagery, buildings, roads, vegetation, streamed collision/AI,
global editor placement and seamless floating-origin travel remain substantial
future work. This does not change the ordinary OFP simulation world.
