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

## Integrated ABI23 runtime checkpoint

Paired source `905ba2dc`, deployed at 11:31, passes the full supported build.
`build/earth-streaming/20261008-113741/result.json` passes the same coastal
checks. Both inspected 1000m/9000m captures now lack the dark grazing sky
band: the Earth sphere-miss interval predicate had incorrectly truncated
atmosphere rays to one metre. Coarse overview imagery, low coastline detail
and the angular source-backed land corner remain visible; this is not
worldwide ocean/terrain visual parity or new collision coverage.

`build/physical-inventory/20261008-113914-007/result.json` passes real SDL
compass down/up, screenshots and clean exit. The inspected open-compass
capture shows the bearing above the instrument; closed captures omit it.
Tab still cycles targets and Shift still controls sprint. Inventory remains
on the configurable O action while the requested conflicting-key choice is
pending; no custom bindings have been migrated.

The ordered/detail-mask material preview is strictly opt-in. Actual Village
on/off runs both settle at 20683 registered instances, 4930 main instances
and 13451410 main triangles. The on log confirms the requested Pub external
material has three real detail masks bound. However, the established Village
camera points away from that Pub, so these images do not establish its visual
improvement. Single-frame engine GPU samples (55.929/54.234ms on/off) are not
a controlled cost conclusion. A separate Pub-facing camera is now available
in `Measure-NativeReforger.ps1 -Pose Pub` for an appropriate comparison.

Scene-depth lifecycle on this installed pair passes in
`build/reshade-depth/20261008-113233-SceneProbe/result.json`: fullscreen
3441x1440 generation2 and return to 1600x900 generation3 both bind fresh
finite scene-depth witnesses, followed by clean teardown. The first metric
DH test correctly failed its strict gate on an upstream same-image
read/write conflict. The generated adapter now retains the original source
outside the effect scan and uses a separate 1x1 Reinhard blend target plus
publish pass. The error gate is unchanged. Eleven Python checks pass with
the actual compiled bridge loaded for contract tests.

With the corrected isolated stage, metric depth debug
`build/reshade-depth/20261008-115017-SceneDHDebug/result.json` and preview
`build/reshade-depth/20261008-115253-SceneDH/result.json` pass compilation,
real depth binding, live off/on, paired captures and clean exit. Debug
silhouettes align with the scene; preview images show a visible effect,
including lifted distant contrast. This is a bounded screen-space prototype,
not hardware ray tracing or final visual acceptance. ReShade stays off by
default. No injected-GPU timing or general MSAA/DLSS compatibility is claimed.

## Pub-facing material comparison

The corrected Pub-facing ordered-mask on/off runs on installed `905ba2dc`
both pass capture lifecycle and settle at 20683 registered / 6295 main
instances and 11826164 main triangles. Receipts are under
`build/native-reforger-perf/Pub-ordered-{on,off}-905ba2dc/run-01.*`.
Inspection finds a small localized plaster/detail change, not a decisive
overall visual improvement. Mean absolute RGB differences in the facade
and gable crops are 0.303 and 0.429 byte values (maximum 8); a neighboring
facade stays within 1 byte. Whole-frame differences also include moving
foliage/sky, so they are not a material correctness test. Single-frame
GPU samples of 56.595/55.031ms are not a controlled cost measurement.

Color filtering and layer mixing still use the existing encoded-color
path in this installed version. The next bounded opt-in correction is
hardware sRGB color-role views and linear layer composition. Normal
composition and lighting remain approximate and require separate checks;
these previews do not establish full material parity.

## Optional sRGB integration checkpoint

Engine source `fd66c9e6` passes the full supported build; the paired install
is stamped `acbe7617` at 12:17 (the additional commit records verification
documentation only). Two actual synthetic GPU tests pass: hardware
decode-before-filter with unchanged alpha, and alias replacement / lease /
eviction / generation / cap handling. No additional pixel storage or texture
sample is introduced; the internal material stride adds 16 bytes and each
eligible unique BCR consumes one capped descriptor.

Pub-facing on/off runs under
`build/native-reforger-perf/Pub-srgb-{on,off}-acbe7617/run-01.*` both pass
lifecycle and settle at 20683 registered / 6295 main instances and
11826164 main triangles. Source admission succeeds and the renderer admits
all six targeted sections. The facade and gable crop differences are at
most one RGB byte, with mean absolute differences 0.026 and 0.027.
Consequently this scene does **not** demonstrate a meaningful visible
improvement, despite the tested filtering/composition correction. Moving
foliage prevents whole-frame deltas from isolating this material change.
Single-frame samples of 55.989/60.704ms are not a speedup or cost claim.
The preview remains disabled by default; normal and lighting approximations
remain open.

The installed classic scene regression check
`build/reshade-depth/20261008-122344-SceneProbe/result.json` passes effect
compilation, live toggling, paired capture and clean exit. Its fresh witness
has 256 finite / 227 nonzero depth values; inspection still aligns terrain,
trees, pole and weapon with the native scene and leaves sky white. This
checks the shared material-layout integration without claiming new ReShade
quality or compatibility coverage.

## Inventory Tab binding follow-up

Installed `25abb7dc` at 12:47 includes engine change `c98b09b8`: Infantry
inventory now opens/closes with Tab; infantry target cycling moves to O.
Shift sprint bindings and vehicle target cycling remain unchanged. Context
profile version 7 migrates the old single O inventory default, retaining
custom/chorded/cleared bindings and refusing conflicting custom Tab/O use.
The supported game and test builds pass. The context-controls suite passes
146 assertions across 13 cases, including migration, roundtrip, conflicts
and vehicle/sprint defaults.

Installed real SDL key tests pass with fresh profiles
(`build/physical-inventory/20261008-124816-711/result.json`) and a v6 profile
(`build/physical-inventory/20261008-124931-122/result.json`). In each, Tab
opens display 62020 and closes back to mission display 46; Shift and O do
not open inventory. Persisted infantry keys and retained vehicle Tab are
checked, the open-inventory image was inspected, and both owned processes
exit cleanly. This supersedes the pending default-key choice above. The
physical inventory remains single-player only; custom bindings remain
configurable in Controls.

## Continued three-task integration

The three implementation tracks remain active. Installed `554c8c44` at 13:48
contains the following source changes; full supported game/test builds pass.
Only the private safety branch was pushed.

- Earth addressing now matches the spherical render projection. Geographic
  free flight rebases its ENU frame at an altitude-dependent 32–400 km
  deadband; finite mission entities remain local. Roads and buildings use
  the same projection. CPU checks include 900 repeated rebases, dateline,
  ECEF position/view/speed, stale revisions and surface alignment.
- Optional pre-tiled HTTPS content cells can follow the resident camera
  window with bounded background transfers and a separate generated cache.
  No worldwide content server is supplied or contacted by default. Real
  synthetic-loopback tests pass byte/JSON/redirect limits, repair,
  cancellation, eviction and failure cooldown. Existing feature/building
  geometry tests pass after the spherical consumer conversion.
- The independent global/detail-normal preview adds 32 bytes per material
  (public ABI24, internal 416-byte material), without new textures/samples.
  The actual production normal helper passes a synthetic GPU readback test
  covering independent detail strengths, ordered overlap and global rotation.
  Fixed-camera material comparison is pending. Authored tangent and complete
  lighting parity are not established.
- ReShade supports nearest-resolved 4x source depth, session-only Graphics
  preview selection and bounded real effect-GPU timestamp windows. Fifteen
  offline checks pass, including the actual staged add-on's publication and
  timing ABI. Actual 4x lifecycle/alignment and GPU cost tests are pending.

The first installed live-selector test on `25a85692` correctly refused its
settings because the gate incorrectly read WGPU's inherited legacy MSAA
getter (zero). It now reads active target dimensions/MSAA via temporal info
and records those settings in the receipt. That failed run is preserved at
`build/reshade-depth/20261008-133235-SceneProbe/result.json`.

The first actual Earth threshold test on `25a85692` preserved global view
direction but failed its strict up-vector check (0.0053388). A later aiming
update reset up to local world-up. `554c8c44` corrects that Earth-only path;
the strict continuity test is unchanged and now starts above Luxembourg
terrain. The failed receipt remains under `build/earth-geodesic-installed`.
These failures are diagnosed, not represented as passing runtime evidence.

The subsequent installed checks on `554c8c44` pass:

- `build/earth-geodesic-installed/20261008-135053-85f917/result.json`:
  actual SDL D crosses the near threshold and publishes fresh matching
  ground; the old-frame query has no stale support. Near global direction
  and up errors are below 4e-8. The orbital arm crosses from longitude
  179.5 to -179.52473939 with direction/up errors about 1e-5, bounded source
  cache and no orbital DEM publication. Its screenshot looks into space;
  it is continuity evidence, not an improved globe-quality image.
- `build/reshade-depth/20261008-134906-SceneProbe/result.json`: actual HDR,
  native 1600x900, MSAA4 and availability-only startup are recorded. No
  depth producer runs before selection. The first fresh witness and resized
  generations 2/3 have 256 finite samples; fullscreen3441x1440 returns to
  window1600x900. Off returns snapshot frame zero. Image inspection aligns
  terrain, foliage, pole and weapon with the scene; sky remains white.
- `build/reshade-depth/20261008-135220-SceneDHDebug/result.json`: 4x
  reconstructed metric depth, exact selected technique, Off release,
  compilation, paired captures and clean exit pass. Debug inspection aligns
  silhouettes and separates out-of-range/sky in blue. This permits the
  subsequent bounded GI/performance check, not final GI quality acceptance.

## Installed tests and actual Worldgen connection

Installed `9672d1aa` (14:16) passes the supported full game/test build. Earth
location acceptance at `build/earth-locations/20261008-142520/result.json`
passes Luxembourg cached roads/buildings/grounded procedural trees, neighboring
cell retention, Alps/Canyon/dateline relocation and stale-revision rejection.
Those trees and buildings are simplified previews, not the upstream Worldgen
stock asset pipeline.

Normal preview on/off at `Pub-normal-{on,off}-554c8c44` has successful targeted
normal admission in the on log (line 8983). Both fixed-camera runs settle at
20683 registered / 6295 main instances. The image difference is modest at this
distance. Synthetic GPU tests pass independent global/detail normals, authored
mirrored/nonuniform axes and per-layer AO clamping before ordered overlap.

The first installed authored-tangent on run at `Pub-tangent-on-9672d1aa` failed:
producer provenance incorrectly replaced MLOD container identity, and the
retained shader exceeded the device fragment input limit when FrontFacing was
included. `ae99a29d` fixes both and invalidates the affected optional cache key.
Its actual 1x/4x device-pipeline test passes, including rejection of an extra
varying negative control. The failed installed run is not performance evidence;
the corrected installed comparison remains pending.

ReShade corrected debug/beauty runs `20261008-141723-SceneDHDebug` and
`20261008-142025-SceneDH` pass lifecycle, 4x depth and clean teardown. The latter
records 240 real GPU brackets: median 2.134528 ms, p95 2.507776 ms; this excludes
engine snapshot/overlay work. Fogged distant geometry still becomes pale and
grainy: native airlight was being donated as surface radiance. Consequently GI
quality was **not accepted**, and the normal local runtime cache was not upgraded.
The integrated compatibility gate explicitly refuses DH GI with active native
fog, while retaining depth debug; fog-free beauty and refusal tests are pending.

Worldgen now has a distinct, bounded real-model placement path, rather than
equating proxy OSM meshes with completed upstream integration. The local
preparation scripts combine existing cached OSM cells and cached Terrarium
elevation and invoke the upstream M4 generator. They make no map-data requests
and write outside the installed game. A narrow M4 stock-bridge dataclass
compatibility shim preserves upstream's existing proxy behavior.

Actual preparation at `D:/OpenPoseidon-Worldgen-Lab-20261008/lux-region-03`
produces 7369 model placements: 1057 roads, 1249 houses, 2950 individual trees,
2066 bushes and 47 forest blocks. CPU parsing passes the actual packaged WRP,
all 7369 resident source bases and bounded selection of 2048 instances, without
skipped model names. Stock model/texture assets remain local game references.
Upstream final road/junction/grading validation reports **failed**; this is a
real asset-presentation fixture, not an accepted playable generated island.
Installed `0e4e1ed4` passes the supported full game/test build. Ground-approach
acceptance at `build/earth-geodesic-installed/20261008-152526-b3e871` uses actual
SDL descent/ascent and holds 0.299987824 metres above current resident ground.
The old 500-metre freefly floor is removed on covered Earth ground.

The first actual Worldgen model test registered trees and houses, but stock
alpha-blended OnSurface road sections were not retained: 1885 created of 2048
attempted. Source parsing alone did not establish successful road rendering.
Dedicated Earth road owners now use the supported cutout/A2C coverage route;
native road ownership stays unchanged. This approximates road-edge alpha and
still requires installed image inspection. Launcher addon roots now resolve to
the direct PBO directory; the earlier startup failures remain in receipts.

A bounded regional OSM PBF importer and offline generated-region index are
integrated. Actual Mamer preparation covers roughly 5.8 by 5.6 km and contains
98421 authored placements. The actual two-region CPU test parses 105790 records
and selects 2048 under the shared instance cap. Upstream road/grading validation
still fails; these are presentation fixtures. Installed boundary continuity,
worldwide source coverage and automatic worldwide generation are not complete.

Installed ReShade tests on `0e4e1ed4` pass active-native-fog GI refusal
(`20261008-154735-SceneDH`) and fog-free 4x depth/GI lifecycle, paired captures,
GPU timing and clean exit (`20261008-155035-SceneDH`). Inspection of the latter
shows subtle ground/foliage illumination without the previous fogged pale hills.
This accepts that bounded fog-free preview scene only; fog-compatible GI and
general visual parity are not established. ReShade remains optional and off by
default.

## Installed ground approach and regional assets, 16:48 build

The matched installed pair is `88098b01`, deployed at 16:48. A fresh actual
SDL ground-approach run passes at
`build/earth-geodesic-installed/20261008-165632-46be6c/result.json`.
Z descends from three metres above current resident ground to 0.299987824 m;
Q ascends again. This removes the old 500-metre freefly floor on covered Earth
ground. Unknown ground still blocks unsafe descent until support is available.
The screenshot is a collision-clearance witness, not a ground-detail showcase.

The two-region installed run at
`build/earth-worldgen-regions-installed/20261008-164932-e254bd/result.json`
passes source limits, region retirement/reload, retained model registration,
current ACT surface publication, camera/ground revisions and clean teardown.
City, Mamer and city-return images were inspected. They show actual stock
buildings and roads over ACT imagery, but the road quality is **not accepted**:
many unclassified road pieces still float, with visible detached shadows.
The near imagery is also visibly blurred. Mamer's source imagery is coarser
than the city source. These failures are kept separate from the passing
residency and resource-lifecycle checks.

Conservative explicit road IDs currently fit a five-sample resident ground
plane, preserving unlisted authored elevated structures. The first classifier
mistakenly treated minor highways which the generator did not emit as positive
road competitors. A bounded classifier correction is being checked; unsafe
bridge/tunnel/layer associations must still veto fitting.

The first installed GlobalMCR preview on this build loads all 20683 instances,
6295 main instances and 11826164 main triangles, but **does not admit the new
target material path**. Its log reports tangent producer3 with admission=false
and global-overlay sourceAdmission=false. Therefore its 54.210 ms frame and
image are not evidence of the new overlay's cost or visual quality. One actual
cause is that the texture importer decodes the MCR data alpha into RGBA8,
preventing the required original linear BC7 admission. This remains under
diagnosis; synthetic GPU contract tests alone do not establish scene parity.

The normal optional ReShade cache was upgraded with the pinned local addon and
licensed files. Installed `748de202` then passed the actual normal-cache
active-fog refusal test at `build/reshade-depth/20261008-163540-SceneDH`.
The previously accepted fog-free beauty pair records about 2.115 ms median
for the GI GPU bracket, excluding engine snapshot/overlay costs. ReShade
remains off by default, and DH GI remains unavailable while native fog is active.
