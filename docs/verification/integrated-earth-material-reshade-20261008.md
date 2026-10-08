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
  follows the geographic relocation. No renderer/device failures and clean exit.
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

The empty Earth fixture can emit the existing `No shape object NOID <no shape>
tested for aiming position` player diagnostic. The camera preview renders and
remains interactive, but this fixture is not character/gameplay acceptance and
its log should not be described as error-free.

Final usability correction: installed `c044d322` at 04:35 local time. Geographic
Go above500km now looks down at the planet rather than reusing the slanted
ground view. The Globe button and harness use that same transaction.
`build/earth-streaming/20261008-043547/result.json` passed actual geographic
orbital Go, screenshot, near-window return and real W/S input; the inspected
`globe.png` shows the complete centered planet. The change is camera-only;
the material, local-surface and ReShade receipts above remain applicable.

## Resident ground, regional imagery and cached features — 8 October follow-up

The matched installed checkpoint `e5e6fc68` at 09:41 contains the resident
height/contact adapter, paired regional imagery, cached landscape features,
altitude-aware sky and near ocean, and an experimental layered-material mask
scale switch. The latter requires both `WGR_REFORGER_PBR_PREVIEW=1` and
`WGR_REFORGER_MASK_SCALE_PREVIEW=1`; normal startup remains unchanged.

The Earth terrain heightmap now publishes an immutable revision/generation
snapshot for local infantry ground queries. It uses the same triangle diagonal
as rendering. Geographic relocation invalidates coverage immediately; uncovered
teleports are rejected and the player is held at the resident boundary. This is
not worldwide vehicle physics, water physics, building collision or navigation.

Actual installed verification:

- `build/earth-ground-installed/20261008-094341-43bb66/result.json`, installed
  `e5e6fc68`: the reported Luxembourg teleport resolves to ground 314.675m;
  standing, walking, crawling, local contact slopes, boundary rejection and
  rapid geographic revision changes pass using a real stock `SoldierWB`.
  Actual external/internal camera state is checked. Inspected third-person
  captures now show the player. The first visual check exposed that Earth's
  draw branch skipped scene object passes 1/2; restoring those passes fixed
  the missing figure. Passing numeric ground checks alone had not proved this.
- `build/earth-locations/20261008-092325/result.json`, installed `c35727c7`:
  Luxembourg near/far imagery, 3x3 cached building retention, all five feature
  kinds, adjacent-window relocation, geographic changes and stale-request
  rejection pass. Actual center window has 1021 paved roads, 1525 unpaved roads,
  438 procedural trees, 19 generic signs and 23 inland-water features, plus
  1995 simple building extrusions. Source feature coverage is only about
  2.2x1.4km; this is not worldwide object coverage or a full Worldgen port.
- `build/earth-streaming/20261008-093211/result.json`, installed `c35727c7`:
  rolling near windows, 30km/120km horizons, orbital geographic Go and actual
  SDL W/S approach/retreat pass. Inspected orbital capture shows scaled clouds
  and catalog stars. Local clouds blend to a simpler planetary presentation.
- `build/earth-streaming/20261008-094943/result.json`, installed `e5e6fc68`:
  Guernsey current ground/horizon publication and actual camera checks pass;
  published FFT diagnostics are active at 1000m and retired above 8km. However,
  the 1000m image exposed a coastal rendering defect: sea-level elevation
  geometry clips wave troughs and leaves foam-like islands. This diagnostic
  PASS is explicitly **not coastal visual acceptance**; the render-only floor
  correction requires a fresh paired deployment and image check.

Luxembourg has three manually prepared external ACT CC0 imagery sources:
2048px center, 4096px district and 4096px context. Sources feather into paired
2048px near/far atlases; edge-connected white NoData pixels are masked without
discarding isolated white roofs. No runtime imagery HTTP was added. Near and
far geometry still share a fixed triangle budget. Resource sizes, preparation
and regional limits are in `docs/earth-local-surface-prototype.md`.

The initial `0153c072` installed smoke test failed during shared water-shader
initialization. `f9e6f5e2` separated Earth-only bindings from the shared wake
module, and the actual production wake/GPU pipeline regression tests pass.
Only subsequent successful installed runs are acceptance evidence.

The layered-material candidate passes three shader tests, including actual
GPU helper readbacks and composition of retained/direct/shadow routes. The
newly built C++ `[mask-scales]` test passes with provenance off/on (9/11
assertions). `Village-mask-off-20261008` and `Village-mask-on-20261008` capture
the same native scene on `e5e6fc68`: 1920x1080, 4xMSAA, no DLSS, fixed camera,
16h, exposure0.5, 60s warmup and 20683 registered instances in each arm. Both
exit cleanly and the intended wall material becomes eligible. Single frame
GPU samples are 56.62/53.81ms, not a speedup or complete cost measurement.
The subtle comparison does not establish the original renderer's layer
contract or visual parity; the candidate remains separately opt-in.

ReShade injection, selected technique compilation, live off/on and final-frame
capture work. Raw depth probes on `c35727c7` with preservation modes 0, 1 and 2
all show a selected but empty depth resource. Depth-dependent GI/RT previews
therefore remain unaccepted and disabled; no beauty comparison or ray-tracing
claim is made. See `docs/roadmap/RESHade-DEPTH-RT-AUDIT-20261008.md` for exact
receipts and the bounded scene-depth handoff still required. Injected GPU work
is outside the engine's timing brackets.

Final paired Earth coastal checkpoint: `7a851e7a`, installed at 10:11. The
render-only coastal floor correction preserves the original resident heights
and avoids most wave-trough occlusion. Integrated numeric and production
water/wake/breaker/shore composition tests pass. The same installed Guernsey
test `build/earth-streaming/20261008-101152/result.json` again passes camera,
publication and FFT lifecycle checks. Visual inspection still finds an obvious
near-ocean/overview transition, a dark corner and differing near/far shading;
worldwide ocean appearance is **not accepted as seamless**. See
`docs/verification/earth-coast-render-proxy-20261008.md` for the elevation
heuristic, possible low dry-land misclassification, exact source and receipts.

Takram source review completed at upstream
`b012ad06d858fc035d88aacfd73f092f93c994e4`; see
`docs/roadmap/TAKRAM-WHOLE-WORLD-REVIEW-20261008.md`. Useful references are
atmospheric/geographic frame consistency and shared bounded tile queues.
Open Poseidon already has dirty-keyed atmosphere LUTs; no duplicate atmosphere,
upstream runtime, assets or costly cloud reconstruction were added. This is
documentation-only research, not new installed rendering or visual acceptance.

## Curved coastal coverage and independent scene depth — later follow-up

Installed `e5b34ac0` at 10:47 removes the resident elevation rectangle from
Earth ocean coverage. `build/earth-streaming/20261008-105311/result.json`
passes the actual 1000m/9000m camera, publication and FFT lifecycle checks.
Eight production selector tests and all four composed water modules pass.
The inspected 1000m image has a substantially more continuous wave field,
but pale water, dark overview land and a dark corner remain visible. This
is improved coverage, **not acceptance of a seamless coastal appearance**.
The subsequent Earth-only physical-irradiance correction requires another
paired installed capture; native OFP lighting and collision heights are unchanged.

Installed `959d1133` at 11:06 adds a separately opt-in world-depth snapshot
before the UI clears depth. The official GenericDepth probes above remain
historically empty; the new `OP_SCENE_DEPTH` semantic is independent of them.
`build/reshade-depth/20261008-110657-SceneProbe/result.json` passes selected
effect compilation, live off/on, capture and clean exit. Its fresh 16x16
bin-centre witness has 256 finite / 227 nonzero values, minimum 0, maximum
0.12434253, and near plane 0.09571429m. The inspected grayscale capture aligns
terrain, tree silhouettes, the pole and weapon with the native image, while
sky is white. This establishes usable scene depth for this Vulkan, 1600x900,
one-sample diagnostic. Resize/rebinding and metric DH reconstruction still
require verification; it does **not** establish a working GI or RT beauty
effect, a total GPU cost, or support for other renderer configurations.

The same installed source shows the bearing readout only while the compass
is open. This UI behavior still needs its own interactive runtime check.
