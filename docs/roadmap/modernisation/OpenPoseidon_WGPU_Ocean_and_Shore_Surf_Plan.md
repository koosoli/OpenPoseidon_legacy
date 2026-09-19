# OpenPoseidon: coherent ocean motion and shore surf

## WGPU-only agent work order

Prepared: 19 September 2026. Revision 1.

**Purpose:** address community feedback that the ocean feels like small-scale pool water or a wobbling elastic surface rather than waves reaching a beach, breaking and leaving foam. Improve observable motion and interaction with the coast, not just surface colour or the number of effects.

**Evidence status:** this is a source-based assessment and proposed implementation sequence. No new build, runtime measurement or side-by-side video validation was performed for this document. The feedback dates from 14 September 2026 and may describe an older build/settings combination. The supplied YouTube reference was identified as a Unity HDRP water tutorial through its metadata and the creator's discussion; its individual shots were not inspected frame by frame. Do not treat it as evidence that Unity solves every kind of breaking surf automatically.

Repository hint: `koosoli/PoseidonWGPU-private-safety-mirror`.
Source-review provenance: remote `codex/squad-ai`, commit `f08ff0d7674e066944f0cf6195ae39e2e94f48aa`.
**Implementation baseline: discover the actual current task worktree, branch and HEAD. None of these historical names is an instruction to switch branches.**

This is a separate water-quality workstream, complementary to the shared streaming plan v4. Do not make it depend on virtualized geometry, Nanite-like work, a new asset pipeline or a GL33 port.

## 0. Working rules and scope

Read the applicable repository/agent instructions and current water/renderer handovers. Inspect repository root, remotes, worktrees, branch/upstream, exact HEAD and uncommitted changes. Redact credentials in reports. The newest remote timestamp, default branch and this document's historical branch are not proof of the owner's active checkout. Revalidate every finding below against the verified baseline; skip already-fixed work. With only remote access, state that local worktree verification was unavailable.

Create any isolated task worktree from the verified current committed development baseline. Do not switch, reset, stash or commit another agent's checkout. Account explicitly for relevant uncommitted changes not present in a new worktree. Pin matched A/B builds, data, settings and shaders; do not silently change the baseline during testing. Respect the normal build/deploy/game-lock workflow and do not overwrite a newer shared installation.

WGPU is the only rendering-development target. No GL33 performance work, shader refactoring, water feature parity or new GL33 test programme. Shared-code compatibility receives only the minimal attention required by existing build contracts. Headless simulation correctness is a separate requirement, not a reason to improve GL33.

Keep current CDLOD water, FFT infrastructure, renderer ownership, terrain sampling, optical model, lakes/rivers, interaction bridge and supported WGPU fallback behaviour. Do not introduce another FFT ocean, foam-history queue or terrain renderer merely because the desired effect sounds new. Do not import Unity/HDRP runtime code or assets indiscriminately; use public documentation as a design reference and respect any dependency's actual licence.

The first implementation slice should be one reproduced consistency correction, or a settings-only change if the audited current code is already consistent. Further surf behaviour is opt-in until visually and technically validated. This plan does not authorize an unrelated physics/network rewrite.

## 1. What the current source already contains

Recheck active code paths and effective runtime settings, not just feature names or historical comments.

| Facility | Source landmark and observed scope |
|---|---|
| CDLOD ocean mesh | `WaterWgpu.hpp/.cpp`; terrain-aware selection, world-space nodes, configurable geometry quality. |
| Spectral ocean | `water/fft.rs`, spectrum/compose shaders; multi-cascade controls and wind/sea-state handling. `WaterWgpu.cpp` contains reference presets and a wind latch intended to avoid rebuilding the spectrum every frame. |
| Analytic fallback | `water.wgsl`: Gerstner displacement/normal functions alongside the FFT path. |
| Local shore displacement | `shore_wave_setup`, `shore_breaker_disp`, `shore_breaker_normal`; existing depth-dependent, shore-directed analytic waves, not a complete breaking/wet-dry solver. |
| Shore data and clipping | `seabed_height`, `seabed_shore_direction`, vertex shore factors, horizontal-motion attenuation and `clamp_to_seabed_height`. |
| Foam history | `foam.wgsl`: previous/next foam, reprojection, advection, diffusion, injection and decay; coupled to the bounded interaction domain. |
| Shore foam and wash | `fs_water`: depth-based foam band, procedural structure, separate sinusoidal swash and shoreline streaks. |
| Optics and detail | FFT normals, micro-normal detail, reflections, refraction, extinction, lighting and foam composition in `water.wgsl`. |
| Gameplay surface queries | `WaterSurfaceQuery.cpp` has a CPU wave approximation and reads some renderer settings. It explicitly does not match the spectral field exactly. Do not rely on old shader comments saying gameplay is always a flat plane. |
| Measurements | Existing water debug modes, freeze controls and pass timers; `WaterWgpu.cpp` publishes water-frame statistics. Reuse them. |

These are source-confirmed facilities, not proof that their current combined output looks right or that every facility was enabled in the community capture. [R1–R8]

## 2. Source-grounded hypotheses to test first

### 2.1 Wave scale is not consistently applied to all wave-associated fields

At the reviewed revision:

- `fft_sample` samples displacement at `xz / wave_scale`, then multiplies its XYZ displacement by `wave_scale`.
- `sample_fft_dynamics_filtered` also samples at scaled coordinates, but `fft_normal_with_weights` divides the sampled slopes by `wave_scale` again.
- `evaluate_water_surface` samples auxiliary crest/compression/variance fields at unscaled `in.base_xz`.
- `foam.wgsl::fft_source` samples the FFT auxiliary field using unscaled world coordinates.

This is an inconsistent sampling contract when `wave_scale != 1`, unless another active producer compensates for it. The mismatch is present in the inspected expressions; its contribution to the user's visual complaint has not been demonstrated. [R3–R5]

For a simple single-band example, if the intended transformation is `h_S(x,t) = S*h(x/S,t)`, its spatial derivative is `h'(x/S,t)`, not that derivative divided by S again. This reasoning is a test oracle, not permission to remove a division without tracing the actual stored field. `fft_compose.wgsl` already applies per-cascade normal/displacement conventions, and horizontal displacement, domain warping, spatial weights and boundary attenuation complicate the final normal. [R5]

Trace coordinate mappings, amplitude units, phase time, normal/displacement scales, compression/Jacobian units and curvature units end to end. Declare the intended meaning of the scale slider: artistic dilation is allowed, but it must be coherent. Changing wavelength while keeping period unchanged changes crest speed; it is not automatically the same physical sea state.

Create a small single-wave/single-cascade test at scale 0.5, 1 and 2 before changing defaults. Compare the actual displaced mesh with normals estimated from finite differences, and overlay the foam source and crest locations. Do not infer correctness solely from a water debug mode: some debug paths sample fields differently too.

### 2.2 A shore-wave surface and a shoreline foam band are not one breaking process

`shore_wave_setup` derives a local direction from the seabed, uses `dot(position, direction)` with a spatially changing wave number, and advances its two waves with independent fixed phase rates. Shore swash elsewhere uses `sin(2*pi*time*swash_speed)`. The persistent foam source inspected in `foam.wgsl` uses FFT compression plus interaction signals; it does not directly consume the analytic shore-wave phase/breaking state. The fragment shader adds another depth-band foam treatment. [R3–R4]

These mechanisms can provide useful effects individually without representing one wave that arrives, steepens, spills, washes inland and recedes. Determine their actual contributions in the current preset before replacing anything.

Test phase continuity where local direction/depth changes. `dot(p, d(p))*k(p)` is not an integrated phase field for spatially varying direction and wavelength. Similarly, a normal that differentiates only the sine phase while ignoring spatial changes in amplitude/direction may not match the surface. Check synthetic translated/curved beaches, a straight beach, map-scale coordinates and LOD transitions. Label any approximation and its supported domain honestly.

### 2.3 The seabed clamp is a safety workaround, not a breaking-wave model

The vertex path attenuates horizontal displacement in shallow water and clamps displaced height to a floor based on terrain and mean surface level. This can prevent specific coastline tears, but it is not a model of wave-energy dissipation, changing water volume or a moving wet/dry front. Do not accept explanatory comments as proof of physical correctness. [R3]

Determine whether it visibly flattens troughs, biases the mean surface or creates a sheet hugging the beach. Retain the established no-hole/no-flood protections until a bounded replacement passes tests. Do not simply delete the clamp and declare the resulting gaps realistic.

### 2.4 Scale, time and material appearance can also explain the feedback

Read effective runtime values, saved developer overrides and active flags. Measure crest spacing, height and travel time against metre-scale markers, a soldier and a boat. Separate long swell, local wind chop and normal-only small detail. An overly broad normal field, mismatched highlight motion, excessive lateral distortion or loss of fine roughness can create an elastic-looking material even with a reasonable geometric wave.

Keep low local wind with incoming swell as a required case. Swell can reach a beach from a distant weather system; shore-breaking energy must not be vetoed solely because current local wind is low. Separate ocean whitecap policy from shore-breaker policy. [E4]

In foam integration, verify the dispatch cadence before diagnosing frame dependence: `foam_update` clamps its dt to 0.033 and includes different per-channel history operations. Test equal elapsed time at several frame rates and dt=0; do not assume existing substepping is absent without tracing the caller. [R4]

## 3. Baseline and smallest first patch

Capture one reproducible beach case using the normal game camera and actual renderer, plus an elevated view and a simple synthetic slope. Include a soldier/boat or calibration posts for scale. Use the same sea state, seed, camera path, FOV, exposure and presentation time for A/B. Prefer clips spanning several wave arrivals over isolated still images.

Start with controls that isolate existing contributions:

1. Current shipping look; default versus the reported developer configuration if available.
2. FFT surface alone, then shore displacement added.
3. Geometry normals versus final shading normals; controlled lighting/reflection contributions.
4. Foam source, persistent history and final foam separately; identify the rendered crest underneath.
5. Swash and shore displacement phase, plus the existing seabed protection.

Reuse existing switches and diagnostic displays wherever possible. Any additional measurement must be narrow, bounded and removable; it must not mutate gameplay state or secretly change the path measured.

**Preferred first patch:** fix a proven scale/normal/foam-space mismatch and add a focused regression test. Preserve independent material normal strength as an explicitly artistic control rather than conflating it with physical wavelength scaling. A failure reproduced only at non-default scale is still useful, but do not claim it explains a default-scale capture.

If all suspected issues are already fixed, record that and choose the smallest measured tuning change or missing surf link. Do not spend the task rebuilding solved systems.

## 4. Sea-state tuning after consistency

Expose or organize physically interpretable controls where the architecture permits: representative wave height, period/spacing, propagation direction/spread, local wind chop, small normal detail and shore exposure. Reuse current controls/presets and document mappings instead of adding another overlapping control panel.

Create separate reference presets for sheltered water, exposed coast with incoming swell, and rougher open water. The objective is not to impose dramatic white surf on every lake and bay. Geometry carries resolvable wave shape; smaller waves contribute appropriately filtered normal/roughness detail.

Keep wave frequency, propagation and scale compatible. For gravity-wave reasoning, finite-depth linear dispersion is `omega^2 = g*k*tanh(k*d)`, with `k = 2*pi/lambda`; the existing reference-wave code already uses this form. Use it as a consistency target within its assumptions, not as a claim that linear theory models an overturning crest. Avoid abrupt spectrum resets when wind, a preset or artistic scale changes. If existing latching prevents rebuild storms, preserve that protection and evaluate an explicit transition rather than multiplying rebuild frequency. [R2]

Do not increase FFT resolution or CDLOD density before identifying a spatial-resolution problem. The desired improvement may be motion/phase coherence rather than a lack of triangles.

## 5. Recommended surf extension: a shared local breaker state

After the baseline fixes, prefer a bounded, locally evaluated surf extension over an island-wide fluid solver. Integrate it into the existing water resources and pipelines. A small set of coherent data is more useful than another collection of unrelated noises.

### 5.1 Inputs and spatial representation

Use validated terrain/bathymetry for local depth, shore slope and shoreward direction, plus incoming swell and a conservative exposure estimate. Screen-space depth remains useful for refraction and contact effects; it must not be the only source of wave propagation or authoritative shoreline direction, which would make surf camera-dependent.

Reuse the terrain height representation and revisions already consumed by water. Check native Reforger resampling and shoreline geometry explicitly: fine source detail discarded earlier cannot be restored just by adding foam. Keep water and visible terrain consistent; improving terrain resolution is a separate dependency only if demonstrated necessary.

Begin on a simple exposed beach. Evaluate stable shore-coordinate/phase data, a baked or lazily derived shore strip, or another small representation that fits existing architecture. Do not orient the entire ocean toward the nearest beach. Broad swell approaches from an incoming direction; only its nearshore response adapts locally. Do not route wave energy through a headland into a sheltered bay using distance-to-shore alone.

Support automatic coarse coverage from available map data, with optional local authoring to improve important beaches. No mandatory manual repainting of every OFP/Reforger island, and no full-resolution new island-wide texture by default. Cache derived coastal data with terrain revision, body identity, relevant parameters and a producer version; late jobs must not install data for an old world or terrain state.

### 5.2 One phase and breaking event shared by consumers

For the pilot, evaluate reusing or narrowly extending the existing `WaterSurfaceState`/shared evaluation functions. First check which fields are actually populated and what their approximations mean; a field named breaking energy or previous position is not proof of a coherent breaking or motion model. The consumers may need local phase, crest envelope, propagation direction, bounded surface displacement, breaking amount and shore wash progression. These are illustrative fields, not a requirement for a new general-purpose framework or a second competing state structure.

A visible cycle should be:

`incoming swell -> locally steeper crest -> spilling/whitewater generation -> landward wash -> thinning/backwash -> wetness decay`

Derive the foam injection and any spray from that wave's breaking state rather than a separate arbitrary clock. Tie broad wash timing to arrivals, with stable variation alongshore. Avoid all beaches pulsing simultaneously.

Treat wave breaking as a depth/steepness/energy condition appropriate to the chosen approximation. Do not identify any point above mean sea level as a breaking wave. A familiar breaker-height/depth heuristic is not an exact universal constant and is not equivalent to clipping troughs to terrain.

Do not add full-amplitude shore waves on top of full-amplitude swell without bounding the overlap. Use a documented transition/replacement or energy-limited contribution; validate the final combined mesh, not the components separately.

The first version should model convincing **spilling surf**, not promise a fully simulated plunging barrel. A single-valued height surface cannot represent an overhanging liquid lip. If a close shot truly needs one, investigate a short-lived local crest strip/ribbon mesh and spray driven by the same event, with bounded topology, culling and rendering cost. Keep that optional; no full-ocean particle/volume simulation is required for the initial goal.

### 5.3 Reuse persistent foam rather than add another simulation

Feed a shore-breaking source into the existing foam/history infrastructure where feasible. Distinguish phase propagation from foam transport: crests travel; foam is material carried by surface flow, drift and surf wash. Do not slide every foam patch at the wave's phase speed indefinitely. [E5]

Foam should be created at active crests, accumulate as whitewater, stretch/spread, thin and decay. Preserve the existing wake and interaction sources. Make diffusion/decay stable in elapsed time and document any simulation-resolution dependence. Do not use a nonlinear display-contrast operation repeatedly on persistent state unless it is a deliberate, timestep-consistent model.

The existing history domain is bounded and camera-following. Preserve world-space history through domain motion, handle uncovered regions, reset on teleport/world change appropriately, and avoid visible rectangular seams. Do not make the entire coastline allocate its own high-resolution foam simulation. A coarse/stateless distant representation is acceptable with a tested transition.

Small breaking-water spray can help, but reuse an existing emitter if the current branch has one. Audit the actual spray path rather than assuming no particles exist because one shader also contains spray-like flecks. Bound births, lifetime, screen coverage and overdraw; avoid broad smoke plumes and unrelated smoke-shadow changes.

### 5.4 Wash and wet beach must follow the same event

Upgrade the independent global swash sinusoid only where the surf pilot owns the shore. Use the shared arrival/breaking progression for a bounded moving wash front, landward extent, drainage and wetness memory. The visual wet layer can be separate from gameplay water volume; it must not silently flood simulation terrain.

First choose and document how the wet/dry front is rendered: main water surface coverage, a local strip/overlay, or existing terrain wetness. Ensure one owner per pixel and avoid stacked translucent surfaces, z-fighting, floating water and gaps. Wetness should linger and dry rather than oscillate instantaneously with one sine. Preserve terrain material layering, shadows, shoreline safety and local water-body levels.

## 6. Gameplay and renderer contracts

Keep surface presentation, local interactions and authoritative simulation deliberately separated. Audit consumers of `QueryWaterSurface*`, water containment/level queries, boat buoyancy, swimmers and underwater-camera gates. The current CPU approximation is not an exact mirror of the FFT. This task must not quietly make camera distance, graphical quality, missing history or GPU feedback alter authoritative water physics. [R6]

A first render-only shore wash is acceptable with conservative limits and a documented mismatch budget. Do not claim boats match metre-high visual breakers if the simulation does not. If close boat interaction requires common low-frequency waves, propose a separate narrowly scoped CPU/GPU sampling contract with tests and stable time/seed—not synchronous GPU readbacks in physics.

Keep ocean effects out of lakes and rivers unless explicitly appropriate. Preserve per-body level, profile, flow, containment and waterline transitions. Account for multiple views and reflection passes so state advances once per intended update, not once per camera.

Use the existing Rust/C++/WGSL layout and producer-window rules. New resources must have valid fallbacks, clear lifetime ownership and generation/revision checks. Reuse existing water pass timers and distinguish resource preparation from rendering cost.

## 7. Acceptance tests and measurements

### Visual acceptance

- A tracked swell advances through the scene at a plausible scale relative to a soldier/boat; it does not resemble stationary blobs merely inflating and deflating.
- At an exposed test beach, identifiable arrivals cause crest-localized whitewater, then wash and decay. Foam does not originate systematically in unrelated troughs.
- A sheltered bay and a lake remain meaningfully calmer than an exposed coast; low local wind does not automatically erase incoming-swell surf.
- Camera movement, FOV and LOD transitions do not change the identity/phase of a wave at a fixed world point or detach foam from its intended source.
- No newly introduced coastline holes, flooded hillsides, hard foam rectangles, repeated bright stripes, obvious tiling or intolerable reflected-highlight crawling.
- Existing boat wakes, interactions, river/lake appearance, underwater view and weather lighting are preserved.

Use current and improved clips at identical settings plus debug overlays. Show improvements to the specific reported issue; a higher saturation or larger foam gain alone is not success.

### Focused technical tests

- Scale 0.5/1/2: consistent displacement, phase-linked auxiliary data and foam source. Compare normals with a finite-difference reference for the supported mapping.
- A synthetic beach translated in world space: no unexplained appearance change from multiplying varying local direction by a large absolute coordinate.
- Straight beach, curved bay, headland, steep cliff, shallow shelf, island edge/off-map ocean, body at non-sea elevation.
- Stationary and moving camera, sudden turn, teleport, world reload, terrain edit and reflection/multiple-camera rendering.
- Equal simulated elapsed time at 30/60/120 presentation FPS, temporary stalls and pause/dt=0. Verify history stability with actual update cadence and substeps.
- Calm and rough presets, wind-direction change, independent incoming swell, feature disabled and unsupported optional-resource path.
- Headless and supported multiplayer behaviour unchanged by graphics toggles; no new renderer dependency in authoritative logic.

### Performance reporting

Report before/after CPU water preparation, spectrum rebuild frequency, FFT/evolve/compose GPU time, foam/interaction time, main water draw, reflection time, new surf pass cost, selected nodes/actual submitted triangles, memory and long-frame statistics. Do not assume a nominal node count equals actual submitted geometry if the current renderer changes its index density.

Set an explicit measured budget for the pilot hardware and coverage before implementation. No invented cross-hardware millisecond promise. Allocate local shore resources only where useful; avoid unbounded per-fragment neighbourhood work or more reflection rendering to disguise a motion issue. Quality reduction may simplify local detail, but must not destroy phase continuity or cause unsafe resource lifetime changes.

## 8. Incremental delivery and agent ownership

**Stage A — verify and correct:** record the live baseline/settings, trace the active paths and reproduce the strongest consistency issue. Deliver one small fix plus tests and matched clips. If disproved, report it and select the next evidenced cause.

**Stage B — tune the existing ocean:** establish metre-scale reference presets and verify geometry/normal/foam coherence. No new solver in this stage.

**Stage C — opt-in surf pilot:** one representative beach, one bounded shared breaker state feeding existing displacement/foam and a controlled wash effect. Prefer convincing spilling surf. Preserve the existing offshore path and coastline safeguards.

**Stage D — broaden only after the pilot passes:** multiple coastal shapes, sheltered exposure, world/terrain invalidation, quality tiers and optional close crest/spray details. Stop or simplify additions that do not improve the footage at an acceptable cost.

Assign one integration owner for `WaterWgpu.cpp`, `water.wgsl`, water parameters and layout changes. A second agent may build regression fixtures and evaluate clips/timings. A third may investigate terrain/shore-data and simulation-query contracts read-only or behind a clearly assigned interface. Do not let multiple agents independently replace wave clocks, coordinate mappings or foam ownership.

Deliver the exact baseline, effective settings, source references, proved/disproved hypotheses, changed files, tests actually run, reproducible captures, measured costs, remaining visual/physical limitations and rollback instructions. Distinguish source reasoning from runtime proof. Do not describe code as physically accurate solely because comments use physics terminology.

**Desired result:** retain the existing WGPU ocean, make its parts agree, and add the smallest coherent shoreline process needed for waves to arrive, break and leave moving foam. Do not replace the engine to imitate a reference video.

## References and evidence provenance

### External primary references

- E1 — User-supplied video, *I tried Unity's new HDRP Water System! The Most Realistic Water?*: https://www.youtube.com/watch?v=xH-9ZEX9n6A . Creator's accompanying discussion: https://discussions.unity.com/t/the-new-hdrp-water-system-is-amazing/919242 . Metadata/discussion identified; not frame-by-frame visual validation.
- E2 — Unity, *The new Water System in Unity 2022 LTS and 2023.1*: https://unity.com/blog/engine-platform/new-hdrp-water-system-in-2022-lts-and-2023-1 . Describes FFT bands and the island scene's local deformers, masks and foam generators. Surface simulation is not a general fluid/splash solver.
- E3 — Unity HDRP 16.0.3, capabilities and simulation: https://docs.unity.cn/Packages/com.unity.render-pipelines.high-definition%4016.0/manual/WaterSystem-Overview.html and https://docs.unity.cn/Packages/com.unity.render-pipelines.high-definition%4016.0/manual/WaterSystem-simulation.html . Version-specific documentation; do not convert its limitations into unsupported claims about every later Unity version.
- E4 — NOAA, *Waves*: https://prod-01-alb-www-noaa.woc.noaa.gov/jetstream/ocean/waves . Wind sea, swell propagation and nearshore transformation.
- E5 — NOAA, *What causes ocean waves?*: https://oceanexplorer.noaa.gov/ocean-fact/waves/ . Distinguishes propagation of wave energy from particle motion and discusses nearshore breaking.
- E6 — NVIDIA GPU Gems, *Effective Water Simulation from Physical Models*: https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models . Geometric and normal-detail scale separation and wave parameterization; design reference, not a replacement requirement.

### OpenPoseidon source inspected at the review SHA

All paths below are relative to the repository. Links are pinned historical evidence; revalidate at the actual task baseline.

- R1 — `engine/WgpuRenderer/WaterWgpu.hpp`: https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f08ff0d7674e066944f0cf6195ae39e2e94f48aa/engine/WgpuRenderer/WaterWgpu.hpp
- R2 — `engine/WgpuRenderer/WaterWgpu.cpp`, especially reference spectra/sea-state handling and water statistics: https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f08ff0d7674e066944f0cf6195ae39e2e94f48aa/engine/WgpuRenderer/WaterWgpu.cpp
- R3 — `engine/WgpuRenderer/rust/src/water/water.wgsl`, inspected ranges 1–955, 1070–1290, 1370–1595, 1610–1845 and 1880–2320: https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f08ff0d7674e066944f0cf6195ae39e2e94f48aa/engine/WgpuRenderer/rust/src/water/water.wgsl
- R4 — `engine/WgpuRenderer/rust/src/water/foam.wgsl`: https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f08ff0d7674e066944f0cf6195ae39e2e94f48aa/engine/WgpuRenderer/rust/src/water/foam.wgsl
- R5 — `engine/WgpuRenderer/rust/src/water/fft_compose.wgsl`: https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f08ff0d7674e066944f0cf6195ae39e2e94f48aa/engine/WgpuRenderer/rust/src/water/fft_compose.wgsl
- R6 — `engine/Poseidon/World/Terrain/WaterSurfaceQuery.cpp`: https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f08ff0d7674e066944f0cf6195ae39e2e94f48aa/engine/Poseidon/World/Terrain/WaterSurfaceQuery.cpp
- R7 — Current native water source inventory: https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/tree/f08ff0d7674e066944f0cf6195ae39e2e94f48aa/engine/WgpuRenderer/rust/src/water . An inventory is not a claim that every listed file was fully audited.
- R8 — Existing water routing previously verified in `engine/Poseidon/World/Terrain/LandscapeRender.cpp`: https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f08ff0d7674e066944f0cf6195ae39e2e94f48aa/engine/Poseidon/World/Terrain/LandscapeRender.cpp . `DrawWater` already delegates to `IWaterRenderer` and returns before legacy water meshes when that interface is available. Do not remove the dispatch call as a supposed duplication fix.
