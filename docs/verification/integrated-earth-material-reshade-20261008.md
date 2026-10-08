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
samples do not establish total ReShade GPU cost. Display-transition and saved
preference acceptance must be recorded separately before claiming those passed.
