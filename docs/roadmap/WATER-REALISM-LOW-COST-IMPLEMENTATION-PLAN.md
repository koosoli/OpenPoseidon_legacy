# Water realism with bounded rendering cost — agent implementation plan

Created: 2026-09-06  
Revision: 3 — explicit boat/object buoyancy and body-aware physics integration, building on revision 2's rendering and cost gates.  
Status: PROPOSED — documentation only; implementation has not been authorised by this document.  
Scope: WGPU ocean, lake, river, shoreline, and underwater presentation, with preservation and body-aware integration of existing boat/object buoyancy.  
Objective: visibly more convincing water while keeping the balanced preset close to current frame time and bounding resource growth.

## 1. Execution contract

Read root [AGENTS.md](../../AGENTS.md), applicable nested instructions, and [installed-game smoke instructions](../../.agents/SMOKE_TEST_INSTRUCTIONS.md) before implementation. The user requested a plan only. A future explicit implementation instruction is required before executing the stages below.

Once authorised, implement the next dependency-ready stage, verify it, record evidence, and continue within the authorised scope. A passing build alone does not complete a visual change. Do not infer permission to publish, merge, overwrite unrelated work, or change gameplay from this plan. This document does not require spawning subagents; one agent can execute the entire sequence.

Rules for every stage:

1. Inspect the current branch and working tree. Preserve existing improvements and unrelated edits.
2. Reconfirm production call sites and runtime settings before changing a system. Comments and old plans can describe superseded behaviour.
3. Keep each slice independently reviewable and reversible. Retain a comparison path or preset during validation; remove obsolete duplicate paths only after acceptance.
4. Make performance gates observable at the final settings application point. Historical environment toggles have been overwritten by C++ settings pushed each frame.
5. Record author verification separately from owner acceptance. Never claim an AI author is an independent reviewer.
6. If visual judgement cannot be established by self-run captures or interactive automation, present the exact remaining comparison to Oliver rather than declaring success.

User clarification: boats and other appropriate objects must have buoyancy. This is an explicit requirement of the planned result, not an optional visual effect. The limited physics integration described below belongs to this plan; engine implementation is still not authorised by this planning conversation.

## 2. Verified starting point and reconnaissance

The source inspection on 2026-09-06 establishes that substantial water machinery already exists. It does not establish current runtime quality, enabled settings, or present-day performance. No fresh game baseline was captured for this plan.

| Area | Existing foundation | Initial entry points |
|---|---|---|
| Ocean submission | CDLOD ocean geometry; producer supplies landscape sea level and ocean flow kind | `engine/WgpuRenderer/WaterWgpu.cpp`, `WaterWgpu.hpp` |
| Surface optics | Fresnel, roughness compensation, physical/legacy look paths, scene refraction, SSR, planar and sky reflection | `engine/WgpuRenderer/rust/src/water/water.wgsl`, `water/mod.rs` |
| Waves | FFT cascades, displacement and slope data; Gerstner fallback | `engine/WgpuRenderer/rust/src/water/fft.rs`, `fft_*.wgsl` |
| Shore and interaction | Bathymetry use, shoreline breakers, persistent foam, interaction field and whitewater | `water/water.wgsl`, `water/foam.rs`, `water/interaction.rs`, associated WGSL |
| Reflection scheduling | CPU projected coverage, completed-frame water visibility, faded activation and adaptive resolution | `engine/WgpuRenderer/rust/src/lib.rs` (`PlanarConfig`, `screen_coverage_of_water`), `water_visibility.rs`, `planar_mips.rs` |
| Underwater | Surface-aware compositor, shadowed lighting volume, FFT-derived caustics | `engine/WgpuRenderer/rust/src/underwater.rs`, `underwater*.wgsl` |
| Gameplay approximation | Deterministic CPU wave predictor, intentionally independent of GPU readback | `engine/Poseidon/World/Terrain/WaterSurfaceQuery.hpp`, `.cpp` |
| Interaction bridge | Existing engine-to-renderer events | `engine/Poseidon/Graphics/Rendering/WaterInteractionBridge.hpp`, `.cpp` |
| Tests | Water lifecycle, altitude, activity and underwater probes | `tests/integration/water/`, `tests/unit/engine/Poseidon/World/Terrain/test_water_surface_query.cpp` |

Paths abbreviated as `water/` above are relative to `engine/WgpuRenderer/rust/src/`.

The main gap is a coherent per-body presentation contract. The inspected ocean producer sets a single sea level and ocean kind. Imported inland water has a separate material history; do not assume that an ocean shader improvement automatically reaches lakes or rivers.

Additional production entry points confirmed during the plan review:

- Imported `CalmWater` classification and fallback: `engine/WgpuRenderer/EngineWgpu.cpp` and `engine/Poseidon/Asset/Formats/Material/EmatMaterialAdapter.hpp`.
- Existing inland ripple/reflection paths: `engine/WgpuRenderer/rust/src/gfx3d/shader3d.wgsl`, `gfx3d/gpu_driven.wgsl`, and `shaders/frame.wgsl`. Reuse or retire these deliberately; do not stack a second water effect over them.
- Water colour snapshot and pass split: `engine/WgpuRenderer/rust/src/lib.rs`, around `has_water`, `water_split`, `set_scene_view` and `Plan3dOp::Water` handling. The existing snapshot is tied to water operations; an inland-only frame needs an explicit supported route.
- ABI declarations: `engine/WgpuRenderer/include/wgpu_renderer.hpp`, `rust/src/ffi.rs`, and repeated WGSL parameter declarations in water/foam/whitewater/FFT shaders. Audit all consumers when changing layouts, including prefix-compatible structures.

The current `QueryWaterSurface` implementation reads `waveAmp`, `waveSpeed`, `waveScale` and `seaStateCoupling` from engine water settings. Those controls are therefore not purely cosmetic. Keep their baseline values and meanings unchanged during presentation tuning unless a separately authorised gameplay change covers the consequences. New normal-detail/foam/optical controls must not feed the CPU predictor accidentally. Also inspect the existing weather-to-spectrum latch in `WaterWgpu.cpp`; do not replace it with spectrum regeneration every frame.

Existing buoyancy is more than a height lookup: `engine/Poseidon/World/Simulation/Collisions.cpp` feeds wave-relative immersion into `GroundWater` contacts, and `engine/Poseidon/World/Entities/Vehicles/Misc/Ship.cpp` applies hull-point forces and torques. The ship's small-hull planing calculation separates mean-surface immersion (`underFlat`) from wave immersion to avoid amplifying swell at speed. Preserve this fix. Audit other users of the shared collision path and the infantry query path before changing its inputs.

Before writing code, trace imported water classification through both retained GPU-driven and direct draw paths. Identify actual material metadata, boundary geometry, elevation, flow information and missing-data behaviour. Report whether each can be reused; do not invent import fields or infer flow from arbitrary mesh UVs.

Read these historical decisions, then verify whether their limitations still apply:

- [Single ocean plane versus imported inland water](decisions/DZ-003-the-water-renderer-is-one-global-plane-so-reflection-belongs-in-the-model-shader-20260814.md).
- [Degenerate imported water UVs](decisions/DZ-005-dayz-water-uvs-are-degenerate-so-the-ripple-is-world-space-20260814.md).
- [Water toggle ownership](decisions/DZ-007-the-water-toggle-was-a-dormant-checkbox-20260814.md).
- [Pond geometry and material pitfalls](decisions/DZ-008-dayz-water-never-dips-and-the-pond-material-was-the-wrong-file-20260814.md).
- [Historical planar reflection cost](decisions/PERF-002-the-arma-3-frame-is-the-water-reflection-20260813.md).
- [Water/transparent ordering and rain response](decisions/RND-042-water-was-composited-over-every-alpha-draw-in-front-of-it-20260822.md).

The older [water rendering plan](../../engine/WgpuRenderer/docs/water-rendering-plan.md) mixes historical proposals and later additions. In particular, refraction and underwater rendering must not be treated as absent merely because an early section describes them as future work.

## 3. Target design

### 3.1 Shared optical model

Use one consistent set of optical properties across surface and underwater paths: RGB absorption, RGB scattering, scattering directionality, roughness, and surface wave character. Separate light travelling through water from reflected light and foam coverage. Derive attenuation from an appropriate path length rather than applying an unrelated blue tint at each stage.

Extend the existing physical path first. Audit scene-linear versus authored colour conversions, reflection weighting, duplicate lighting, refraction validity, and surface/underwater parameter correspondence before adding new passes. Preserve reverse-Z and farthest-sample depth conventions where required. Distinguish vertical depth used for shore classification from optical distance along the view ray.

Define units and compositing explicitly: absorption and scattering coefficients are non-negative inverse metres; extinction is their sum, with transmittance `T = exp(-(absorption + scattering) * water_path_metres)`. Scattering source radiance must account for the light available along that path; extinction alone is not a complete water colour model. Handle zero coefficients without division by zero. Migrate existing artistic controls through an explicit compatibility mapping instead of silently reinterpreting saved settings.

Audit the final blend state as well as shader equations. The current surface samples scene colour but also returns alpha; any new formulation must state whether its output already contains transmitted background. Do not mix that background into the result twice through shader transmission and framebuffer alpha. Keep geometric coverage/shore softness distinct from optical transmission. Use a small analytical fixture to check zero path, increasing depth, zero scattering and reflection/transmission limits before visual tuning.

The reference is Epic's [Single Layer Water](https://dev.epicgames.com/documentation/en-us/unreal-engine/single-layer-water-shading-model-in-unreal-engine): a dedicated water pass combining scattering, absorption, reflection and refraction, with optional reduced-resolution inputs and simpler fallbacks. Adopt the useful principles within this engine's frame graph; a conversion to Unreal's GBuffer layout or a complete deferred water rewrite is not a prerequisite.

### 3.2 Per-body presentation data

Propose names and layouts only after tracing existing ABI and import structures. The logical data contract should contain:

- Stable body identity, world ownership and lifetime/generation information.
- Body type: ocean, lake, river; explicit compatibility fallback for unclassified water.
- Surface representation: ocean plane, bounded lake mesh/region, or river mesh/reach with varying elevation.
- Bounds and a containment query suitable for camera volume selection; surface height alone is insufficient.
- Optical profile and bounded wave/displacement parameters.
- Flow direction/speed or a flow-map reference with a documented coordinate convention.
- Optional shallow-depth/bank-distance data, and explicit validity flags when absent.
- Selection priority and deterministic overlap rules for connected or overlapping bodies.

Keep immutable body metadata separate from frame-varying weather and interaction inputs. Use compact profile indices or shared buffers instead of cloning resources per draw. Specify ownership across the C++/Rust boundary, alignment/size assertions, index bounds and resource retirement. Respect any newer render-snapshot architecture present when implementation begins.

Reuse imported surface meshes first. A new spline editor or universal mesh generator is not required for this scope. Ocean CDLOD remains appropriate for large sea surfaces. A flowing river cannot be represented by expanding the global sea plane.

Identify bodies from verified instances and connectivity, not material names alone: multiple unrelated lakes may share a material, and one river may span many meshes. Cache spatial lookup and any topology work at registration/streaming time. Do not scan all triangles or rebuild a map-wide flow field each frame. Open/non-manifold surfaces and missing boundaries require a documented fallback; a material flag is not proof of a closed underwater volume.

### 3.3 Body character

| Body | Intended appearance | Low-cost representation |
|---|---|---|
| Ocean | Wind-driven chop over swell; bright but stable glints; foam at breaking crests and shore | Existing FFT and CDLOD; retain current baseline resolutions |
| Lake | Smaller wind ripples, calm sheltered zones, depth-dependent clarity, little surf | Shared normal detail; small bounded displacement only where useful |
| River | Motion follows the channel; reduced flow near banks; local streaks and turbulence; no ocean swell | Authored/reused flow field with two-phase normal/foam advection and short local ripples |
| Underwater | Visibility and colour match the selected body; believable surface crossing and sun response | Existing bounded volume and caustic resources, selected for the camera's body |

Epic's [water meshing documentation](https://dev.epicgames.com/documentation/unreal-engine/water-meshing-system-and-surface-rendering-in-unreal-engine) is a reference for shared rendering of distinct water-body shapes and smooth LOD. It is not evidence that this repository already supports those body definitions.

### 3.4 Rendering-pass and depth contract

Treat this as a prerequisite to full inland optics, not final polish. Produce a short resource/pass diagram during WRL-003a with explicit read/write ownership:

1. Establish the opaque background colour/depth used for transmission. Migrated water must not masquerade as the seabed in those inputs or occlude the seabed during opaque culling.
2. Submit migrated surface geometry through a water-aware path. Remove its old colour/prepass participation only when the new path is ready; unsupported bodies retain their previous rendering.
3. Share a valid background snapshot across compatible water batches in a segment. Do not copy/resolve the scene once per body. Never sample the active render attachment as the refraction source.
4. Keep background opaque depth separate from water-surface depth/identity when a consumer needs both. Add a water mask/depth target only if existing resources cannot carry the required information and its cost fits the budget.
5. Define which water surface wins at overlapping pixels. Connected bodies share an explicit boundary; arbitrary stacked transparent water layers are outside the single-layer approximation. Document the fallback for unsupported overlapping volumes rather than silently compositing twice.
6. Preserve the water/cloud/transparent ordering established by RND-042, including multiple operation segments and weapon depth clears. Inventory prepass, main colour, reflection, shadow, velocity and culling classification; shared shading alone does not route any of those passes correctly.

A frame with only an elevated lake and no submitted ocean is a mandatory early fixture. It must obtain valid colour/depth inputs and underwater activation without depending on an ocean draw or stale sea-level parameters.

### 3.5 Finite underwater paths and missing information

Camera containment selects likely water resources; it does not determine the water length of every screen ray. For lakes and rivers, clip attenuation/scattering to the actual wet interval between entry, exit, bed/opaque hit and the body's finite lateral bounds. A ray can leave a lake through its side while remaining below its surface elevation. Dry tunnels/cavities below an elevated surface must not become water merely because they lie within its horizontal bounds.

Distinguish three inputs: opaque view-ray hit distance for transmission, world-space local bed/bank data for persistent waves/flow, and surface geometry/volume bounds for water entry/exit. Screen-space depth is camera-dependent and cannot replace bathymetry or containment. Define what happens when each input is unavailable. Begin with a bounded height-field/mesh representation supported by the asset inventory; do not add a general volumetric solver.

Use one explicit owner for each attenuation segment so the surface pass and underwater compositor do not both integrate the same water. Specify behaviour for an above-water eye, a submerged eye looking down, a submerged eye looking through the surface, and a near-surface eye looking outside the body's boundary. When a ray intersects multiple distinct bodies, either handle a fixed bounded number of intervals or document a tested nearest-body approximation; never apply the camera body's colour to the entire ray by default.

### 3.6 Buoyancy and rendered-water agreement

Boats and object classes already configured to float must remain supported by the local water surface, with stable heave, pitch and roll. Preserve mass, hull/contact-point configuration, drag, grounding and intended damage/sinking behaviour. Do not make all objects float simply because they touch water. Additional object classes need explicit existing or newly documented buoyancy configuration, not material-name heuristics.

Extend the existing CPU query/contact integration to identify the body at each relevant hull point and return valid local water information: body identity, mean surface elevation, approximate wave elevation/normal, and any explicitly supported physical current. Reuse simulation-owned body geometry and deterministic parameters; GPU-visible bodies and camera-dependent LOD must not decide whether a boat floats. Queries must work for headless simulation and off-screen boats without the WGPU renderer or GPU readback.

For an elevated lake, use its local mean surface for both immersion and `underFlat`. For a sloping river, evaluate the local mean at each contact point. Do not feed a lake's absolute elevation difference from the ocean into the ship's wave/planing term. Keep solid-bed collision and grounding ahead of inappropriate water support, and make transitions at banks and connected mouths continuous where the authored geometry is continuous. A renderer-only body lacking reliable physics bounds remains explicitly unsupported for boating until the query/contact integration is verified.

The CPU predictor need not match every FFT crest, but large-wave height, direction, period and boat waterline must agree well enough to avoid sustained hovering or hull submersion. Establish the current mismatch in WRL-001 and record an acceptable hull-relative error for representative boats before changing waves. If necessary, keep a shared low-frequency carrier for physics and visible large waves while leaving small ripples cosmetic. Do not kinematically snap boats to the rendered surface or count wave lift twice.

Use a fixed, bounded contact/query budget per active object, spatially indexed body lookup and the existing simulation timestep. Quality presets and render frame rate must not change buoyancy. Audit how authoritative wave/body settings reach server and clients; do not claim cross-client agreement from a deterministic formula alone. Keep river texture-flow speed separate from physical current unless deliberate current forces are included and validated; visually flowing water does not automatically authorise a new drag/propulsion model.

## 4. Performance contract

These are proposed acceptance limits, not measured results. Confirm the representative hardware and baseline in WRL-001; do not silently weaken limits after implementation.

- Balanced preset target: neutral total GPU frame time. Maximum acceptable median increase per representative GPU-bound scene: 5% at identical resolution, camera, weather and reconstruction settings. This is cumulative against WRL-001, not an allowance for each package; measure both the previous accepted stage and the original baseline.
- Record p95 frame time as well as median. A repeatable p95 regression above 5%, new periodic stalls, or unstable allocation spikes requires investigation before acceptance.
- Record CPU frame time and submission time; GPU savings do not justify a new CPU bottleneck. Investigate any repeatable CPU regression above 5% in matched CPU-bound scenes.
- Establish explicit byte caps for additional render targets, flow maps and nearby simulation pools after the allocation inventory. Final rollout cannot pass with an unspecified memory budget.
- Metadata can scale with body count; expensive simulation, reflection and underwater resources must scale with a fixed active budget, not total map water-body count.
- No synchronous GPU readback, per-frame allocation churn, larger default FFT, or blanket increase in ocean tessellation.
- Preserve off-screen/occluded work suppression. Default to at most one selected planar reflection view per frame unless profiling demonstrates another fixed budget is affordable; never one view per lake.
- On weak hardware, degrade reflection detail and distant effects before breaking water boundaries, body identity or depth correctness.

Freeze a per-scene absolute millisecond allowance alongside the percentage gate in WRL-001. A slow unrelated scene must not grant water an arbitrarily large budget. Apply whichever limit is tighter, account for all water-related work (including copies, resolves, culling and volume work), and do not sum overlapping timer containers and their leaf regions. Keep frame generation off for timing and compare native and reconstructed rendering separately; also capture the actual dynamic/internal resolution rather than only the output size.

Baseline equivalence matters: improved inland water will replace a cheaper, less capable material on some maps. Still measure the incremental cost against that original map rendering; do not claim neutrality by comparing only against an artificially expensive intermediate implementation. Any proposed relaxation remains a pending scope/performance decision, not a passing result.

Measure alternating baseline/candidate runs with at least three matched rounds, fixed warm-up and sufficient steady-state frames for percentiles. Keep unrelated builds and workloads idle. Record resolution actually used, hardware, driver, executable and DLL fingerprints, flags, weather, camera and raw timings. If variation obscures a 5% decision, report uncertainty and improve the measurement rather than claiming a pass.

## 5. Work packages and dependency order

IDs below are local to this plan. They are not additions to the Preview-0 authorised ticket set.

Deliver three independently useful milestones: **A** = ocean optical/foam polish (WRL-001/002); **B** = one complete lake path (WRL-003/003a, lake portion of 004/005/006 and buoyancy package 006b); **C** = river flow, body connections and full rollout. Finish a lake from submission through underwater, boat support and cost measurement before generalising every subsystem to rivers. A blocked imported river must not prevent acceptance of already verified ocean improvements.

WRL-003a is an added prerequisite inside the existing numbering. WRL-005's ocean-only budget work can move directly after WRL-001 if the baseline shows that it is necessary to fund the other stages. Schedule by measured dependency, not by an assumption that reflections can wait until the end.

### WRL-001 — Baseline and production-path inventory

Dependencies: future implementation authorisation.

1. Inspect active water defaults, FFT resolution/cascades, physical-look selection, MSAA, planar sample count, refraction inputs and temporal integration.
2. Trace ocean and imported inland water submission, shading and underwater activation end to end.
3. Capture the test matrix in section 6. Include one real imported lake and river when assets are available; otherwise record the asset blocker and use a clearly labelled synthetic fixture for development.
4. Inventory existing GPU timers and memory allocations. Add only missing instrumentation needed to distinguish surface, reflected scene, simulation, foam and underwater costs.
5. Establish named baseline presets and per-scene image/measurement evidence.

6. Record three to five specific visual defects using matched crops/sequences and measurable reproduction conditions. For example: foam persists in deep calm water, a reflected shoreline moves when pitching, or the same lake has inconsistent visibility above/below. Do not assume an old capture represents current output. Use these defects as the visual acceptance checklist, and preserve already accepted appearance outside those changes.

Exit: reproducible baseline, verified feature table, body-data availability report, concrete resource caps, and a ranked list of visible problems. Do not optimise based only on the historical 7.94 ms planar ablation.

### WRL-002 — Surface and shoreline polish

Dependencies: WRL-001.

1. Tune existing physical shading for coherent shallow/deep colour, stable glints and plausible roughness. Compare calm, windy, overcast, low-sun and night scenes.
2. Audit wavelength filtering and lost-slope variance compensation before adding detail. Keep long swell visible at altitude without introducing distant sparkle.
3. Adjust foam generation, decay and coverage using existing breaking/shore inputs. Avoid a continuous white ribbon and persistent tan/gritty coverage over the entire sea.
4. Preserve lit foam, seabed constraints, coast softness, wet-band behaviour and current rain response. Missing depth must not create false shores.
5. Prefer parameter or arithmetic changes over additional texture reads. Profile shader variants that materially increase sampling.

6. Keep geometry amplitude/period controls coupled to gameplay at their accepted defaults. Tune normal detail, optics and foam independently; verify that CPU surface samples at fixed inputs remain unchanged. Distinguish quality-preset changes from intentional user sea-state changes.

Exit: matched captures show the targeted improvements, performance passes, and existing ocean/interaction tests remain intact. This stage should be independently shippable.

### WRL-003 — Body metadata, lifecycle and selection

Dependencies: WRL-001; normally execute after WRL-002.

1. Define the minimal presentation contract from section 3.2 and route a default ocean through it with equivalent output.
2. Register one bounded elevated lake and one sloping river fixture. Preserve authored positions and prevent duplicate original-plus-new water draws.
3. Implement conservative bounds, culling, camera containment and deterministic overlap/connection handling. A camera below lake elevation but outside the lake must remain in air.
4. Handle map changes, body removal, origin shifts, teleports and resource reuse without stale identities.
5. Preserve existing ocean query/force behaviour while providing the simulation-owned body data needed by WRL-006b. Rendering may consume existing simulation state; it must not replace the CPU predictor with GPU readback or make body availability depend on visibility.

Tests: body containment at banks and boundaries, river height interpolation, overlap priority, invalid profile handles, removal/reload and ABI layout. Exercise both imported draw routes.

Exit: default ocean parity; correct elevated lake and sloped river placement; deterministic body selection; bounded metadata/resource ownership. Body registration alone does not prove buoyancy support.

### WRL-003a — Inland draw migration and one-lake vertical slice

Dependencies: WRL-003.

1. Implement the pass contract in section 3.4 for one bounded lake using simple shared optics, before adding river animation or elaborate profiles.
2. Prove that the bed remains in the opaque snapshot, the old imported surface is drawn exactly once through its replacement, and both retained/direct routes agree.
3. Exercise an inland-only frame, a lake plus ocean at different elevations, and transparent geometry before/after water operations. Check 1x and the supported MSAA path, with and without reconstruction.
4. Establish finite lake path/containment data from section 3.5 and carry body identity far enough for the underwater compositor to select the lake. Complete the lake portions of WRL-004/005/006 before expanding this path to all imported bodies.
5. Preserve a per-body compatibility fallback when a surface cannot be migrated; failure of one import must not disable all water or alter arbitrary reflective materials.

Exit: end-to-end lake evidence, correct source buffers and pass ownership, no dependency on ocean submission, no duplicate draw, and a measured allocation/pass-cost delta. Metadata-only tests cannot satisfy this gate.

### WRL-004 — Shared lake and river shading

Dependencies: WRL-002, WRL-003a. Complete the lake slice first; river generalisation follows the accepted lake milestone.

1. Route inland surfaces through common optical helpers or equivalent shared material logic without broad unrelated shader refactoring.
2. Implement profiles for clear lake, turbid lake, slow river and faster shallow river. Defaults must be conservative when authoring information is absent.
3. Use world-space coordinates or verified authored flow coordinates. Blend two offset advection phases to avoid visible reset pulses and texture stretching.
4. Derive bank slowdown from reliable bank-distance/flow data; do not mistake screen-space depth gradients for a persistent river velocity field.
5. Add low-cost foam streaks and local obstacle turbulence only where inputs support them. Use a bounded nearby interaction pool; avoid allocating FFTs per body.
6. Blend visual properties and flow at explicit connected mouths. Preserve surface height and prevent overlapping surfaces from double-compositing.

7. Define how rain/projectile/wake events are assigned to body identity and surface elevation. Reset or reassign pooled state on eviction/teleport without leaking ripples between nearby disconnected lakes. Preserve current ocean event semantics. On pool exhaustion, reduce visual detail predictably rather than allocating beyond the cap.

8. Separate low-frequency drift of foam from short ripple propagation; scrolling a normal texture does not transport an interaction simulation. Missing flow or bank data uses a stated modest fallback. Sheltered lake variation and obstacle turbulence are conditional on available authored/derived inputs, not a reason to build a fluid solver.

Exit: river motion visibly follows bends; lakes retain their own elevation and calmer character; seams, UV repetition and body transitions pass moving-camera checks.

### WRL-005 — Reflection and refraction budget

Dependencies: WRL-003a and the relevant body slice of WRL-004. Profiling findings may justify an earlier ocean-only slice after WRL-001.

1. Preserve and extend current coverage/visibility controls. Select the most valuable compatible planar plane using coverage, roughness and view relevance, with hysteresis.
2. Use sky reflection as a continuous fallback, bounded SSR where valid, and planar reflection within the global budget. Distinct elevations must never sample an incompatible planar transform as if it were their own.
3. Verify current reflected sample counts before changing them. Experiment with single-sample or cheaper reflected clouds only if measurements identify a benefit and quality remains acceptable.
4. Consider reduced-resolution refraction/SSR only as measured experiments. Include copy, resolve and filtering cost in the comparison.
5. Validate depth at displaced refraction samples. Reject foreground or invalid samples and fall back gracefully at screen edges/disocclusion. Do not solve this by adding an unconditional second full scene render.
6. If caching reflections across frames is attempted, require reprojection/validity checks and invalidation on camera cuts, body changes, resize, lighting changes and origin shifts. Do not simply reuse an old image under a moving camera.

Exit: reflection transitions do not pop; calm lake reflections remain credible; refraction does not drag weapons/shore objects into the water; total cost meets the contract.

### WRL-006 — Per-body underwater rendering

Dependencies: WRL-003a and the relevant body slice of WRL-004; integrate with WRL-005 before final validation.

1. Select the actual containing body and local surface instead of relying solely on global sea level. Use the same optical profile as the surface.
2. Extend the current displaced-waterline treatment to inland geometry, including partial screen submersion. Avoid a whole-screen on/off colour switch.
3. Audit attenuation and scattering so the scene is not fogged twice. Preserve shadows and handle looking from underwater through the surface toward air.
4. Validate underside normals and total internal reflection; add missing behaviour only after checking the existing compositor.
5. Reuse the existing lighting volume and caustic allocation. For bodies without FFT data, use shared low-cost ripple-driven caustics or a subdued fallback, not a new FFT.
6. Gate caustics by depth, visibility and available light; avoid projecting them onto unrelated above-water geometry. Reset history and active resources when bodies change.

7. Validate finite ray intervals, side exits and dry cavities from section 3.5. Separate visual camera submersion from player/gameplay submersion so free cameras, vehicles and spectator views use their own view. A gameplay hint must not force a dry camera's whole screen underwater.

8. Ensure low-cost lake normals and caustics do not keep the ocean FFT running solely to satisfy bindings when no ocean consumer needs it. Distinguish lazy allocation, active dispatch and retained cache memory in the measurements.

Exit: ocean/lake/river transitions are correct from both directions, clarity matches surface appearance, no inland water fog leaks outside boundaries, and underwater work stays within budget.

### WRL-006b — Boat and buoyant-object integration

Dependencies: WRL-003 and the relevant lake/river surface slice; execute alongside that body's visual integration rather than waiting for all underwater work.

1. Capture existing ocean behaviour for a small planing boat, a larger displacement boat and another already-supported floating object/vehicle where available. Include the existing damaged/sinking state and a non-buoyant control object. Record unavailable fixtures rather than claiming coverage.
2. Extend water-contact lookup to the correct local body without replacing the existing hull-point force/torque model. Preserve the mean/wave immersion split and audit remaining global `GetSeaLevel` assumptions in ship contacts, wakes and placement helpers that affect the tested scenario.
3. Verify the same hull at rest and moving on an elevated lake, a sloping river, a bank/shallow bed and a connected mouth. Do not disguise missing physical support with render-only position offsets.
4. Check high-speed stability, turns, reverse, partial grounding and damaged sinking; verify wake origin and body assignment follow the actual boat waterline.
5. Check fixed-step results across render quality/frame-rate changes, off-screen simulation and the available headless/server path. Preserve authority and replay behaviour; record limitations of multiplayer verification explicitly.
6. Measure CPU query/contact costs with multiple active boats. Keep changes bounded and maintain original ocean regression tests as well as new inland-body tests.

Exit: eligible objects float on supported bodies with stable heave/pitch/roll, no planing regression, no ocean-level snap on inland water, preserved grounding/sinking, acceptable visual waterline agreement and bounded CPU cost. Lake/river boating cannot be marked complete from screenshots of an empty surface.

### WRL-007 — Rollout and acceptance

Dependencies: WRL-002 through WRL-006, including WRL-003a and WRL-006b.

1. Define Low and Balanced presets; keep experimental expensive features outside the default. Low retains correct water-body boundaries and optics while reducing reflection and detail work.
2. Verify native rendering and supported temporal reconstruction modes at their real internal resolutions. Check moving foam, wakes, surface motion, reflections and disocclusion for ghosting; do not assume motion vectors describe animated water correctly.
3. Run the full matrix and lifecycle checks. Check zero visible bodies, many distant ponds, repeated body entry/exit and long traversal for retained-resource growth.
4. Document supported imported body types, missing metadata fallbacks, exact quality settings and measured cost. Do not describe synthetic fixture coverage as real-map validation.
5. Remove temporary diagnostics only after preserving reproducible evidence and useful permanent counters.

Exit: all correctness and performance gates pass, remaining visual decisions are explicitly accepted or recorded as pending, and implementation status is accurately reflected in the appropriate ledger.

## 6. Validation matrix and build workflow

| Scene | Required observations |
|---|---|
| Open ocean, eye level and aerial | Long swell, stable horizon/glints, no LOD popping; calm and strong wind |
| Shallow beach and steep coast | Depth colour, foam placement, wet edge, no ground holes or false foam |
| Elevated clear lake | Correct boundary/elevation, visible shallow bed, plausible reflections |
| Turbid lake | Shorter visibility consistently above and below the surface |
| Sloping river with bend | Downstream advection, bank behaviour, no ocean swell or phase-reset pulse |
| River mouth | Smooth connection, no double surface or colour discontinuity |
| Underwater crossings | Slow/fast crossing, wave crest/trough, bank exit, camera half submerged |
| Finite volume boundaries | Side exit below surface elevation, dry cavity under lake, rays through two bodies; no extra extinction |
| Inland-only frame | No ocean submitted; correct lake snapshot, activation, reflections and bounded allocations |
| Camera versus player | Free/spectator/vehicle camera above water while player is submerged, and the reverse |
| Rain and interactions | Rain streaks, ripples, projectile/boat wakes, bounded foam response |
| Buoyancy | Small/large boats and supported floating objects at rest/speed; stable heave/pitch/roll on ocean, elevated lake and sloping river |
| Boat boundaries and damage | Bank grounding, shallow bed, connected mouth, turning/reverse, high-speed planing, intended sinking and non-buoyant controls |
| Physics independence | Same fixed-step behaviour across quality/frame rates; off-screen and headless support; bounded multi-boat CPU cost |
| Transparent foreground | Shore foliage, fence, smoke, glass, weapon; correct compositing and refraction rejection |
| Reflection stress | Camera pitch, grazing view, off-screen objects, changing body/plane priority |
| Lighting | Noon, sunset, night, overcast; no luminous unlit foam or caustics |
| Lifecycle and temporal | Resize, minimise/restore, teleport, origin shift, world reload, supported upscalers |
| Resource stress | Hidden water, many small bodies, extended travel, resource pool exhaustion |

Add analytical/behavioural checks where applicable: monotonic transmittance versus wet path, no attenuation on a dry ray, unchanged fixed-input CPU surface samples, exactly one migrated surface contribution, and stable resource counts after repeated load/unload. Test numerical limits at zero flow/absorption, grazing rays and invalid/absent depth. Avoid tying tests to incidental source strings or chosen helper names.

Use [Windows build instructions](../build/win.md) and [test documentation](../../tests/README.md) with the active configured build directory. Inspect runner parameters before invoking them; do not invent CMake targets, test names or capture options.

For changes requiring runtime verification, follow the installed-game instructions, including deployment of the matching executable and renderer DLL. Their documented installation is `D:\SteamLibrary\steamapps\common\ARMA Cold War Assault`; reconfirm it before deployment. Honour tool permission requirements for external writes and launches.

Existing runner entry points, to be verified against the current script:

```powershell
.\scripts\run_preview0_wgpu_check.ps1 -Backend wgpu
.\scripts\run_preview0_wgpu_check.ps1 -Backend wgpu -LifecycleSmoke
.\scripts\run_preview0_wgpu_check.ps1 -Backend wgpu -MissionCaptureSmoke
```

Reuse integration scenarios in `tests/integration/water/` and add focused scenarios for body boundaries and river/lake transitions. Shader compilation, ABI checks and behavioural unit tests support runtime evidence; string-presence assertions alone cannot prove rendering correctness. A generic training screenshot does not replace the water-specific matrix. A still image does not establish animation or temporal stability: capture a sequence or conduct a repeatable moving-camera run.

## 7. Evidence, ledger and handoff

Store evidence under `docs/roadmap/evidence/` using a unique WRL stage/date prefix. Each completed package should provide:

- Baseline/candidate commit or working-tree fingerprint, executable/DLL identity and actual settings.
- Exact reproduction command, map/assets, camera, weather, warm-up and sample interval.
- Before/after images, moving-camera observations and raw timing data.
- Median/p95 frame and relevant pass times, CPU data and peak/incremental memory.
- Tests/build commands with results; known limitations and unverified claims.
- Changed interfaces, dependencies, rollback route and next dependency-ready package.

Respect [renderer-systems-ledger.yaml](renderer-systems-ledger.yaml) for shipped-system evidence and its validator. [status-ledger.yaml](status-ledger.yaml) is the Preview-0 ledger with a constrained authorised ticket set: do not append WRL IDs to it as though they were already authorised Preview-0 tickets. At implementation start, identify the active ledger convention for this scope and record the work there without changing governance or asserting owner validation. This plan remains PROPOSED until execution is actually authorised; documentation creation does not complete WRL-001.

Suggested stage handoff record:

```text
Package: WRL-00x
State: proposed / in progress / implemented / verified / blocked
Scope and changed files:
Baseline and candidate fingerprints:
Tests and runtime evidence:
Performance and memory comparison:
Known limitations / owner judgement pending:
Rollback:
Next package and prerequisites:
```

## 8. Explicit exclusions and stop conditions

Exclude full fluid simulation, general shallow-water solvers, waterfall rendering, ray-traced reflections, higher default FFT resolution, a new water authoring editor, and wholesale hull-physics/network redesign. These require separate scope and evidence. Preserving existing buoyancy and extending local body queries/contact support under WRL-006b are explicitly in scope; new physical river-current forces are not implied. Preserve the existing CPU gameplay approximation unless the bounded waterline-agreement work requires a documented compatible adjustment; older documentation saying gameplay is always a flat plane is not a reliable description of the current code.

Stop the affected package when required asset metadata is genuinely unavailable, a reproducible external blocker has no safe alternative, an unresolved safety/external-authority decision is needed, or required visual judgement cannot be automated. Diagnose the specific blocker and retain completed independent work. Do not substitute a global sea plane for inland bodies, silently increase budgets, or mark a stage verified because it compiles.

The intended result is a coherent, scalable water system with distinct ocean, lake and river character and matching underwater optics. Success is demonstrated by matched visual evidence and measured resource use, not by the number of effects enabled.
