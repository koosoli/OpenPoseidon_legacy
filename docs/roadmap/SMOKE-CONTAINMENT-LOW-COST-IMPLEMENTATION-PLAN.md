# Smoke containment, open doors and low-cost visual improvement

Created: 2026-09-06  
Revision: 2 — reuse the integrated Box3D query service where coverage and measured cost support it.  
Status: PROPOSED — plan only; no engine implementation authorised by this document.  
Goal: smoke stays on the correct side of solid walls, floors, ceilings and closed doors, can travel through actual open doorways, and looks more natural without a large resource increase.

## 1. Execution and scope

Read [AGENTS.md](../../AGENTS.md), current applicable instructions and [installed-game verification instructions](../../.agents/SMOKE_TEST_INSTRUCTIONS.md) before future implementation. Continue dependency-ready work once implementation is authorised; do not stop merely because a build passes. This plan does not require multiple agents.

Extend the existing particle/interior systems. Do not start with a full fluid simulation, a world-sized smoke volume, ray tracing or a replacement physics engine. Containment and visual transport are required; gameplay visibility, AI perception, damage, oxygen and network-authoritative smoke density are separate scope. Smoke is not to become solid collision geometry.

The requirement has two independent parts:

1. **Transport containment:** particle motion cannot cross a solid boundary; an open doorway permits travel between its actual adjacent spaces.
2. **Rendered containment:** the visible footprint of a puff must not extend through a wall even while its centre remains in the correct room.

Depth fading alone cannot solve the first problem, and collision of the particle centre alone cannot solve the second. Acceptance requires both, viewed from inside and outside and during camera movement.

## 2. Current code and what must be retained

Source inspected on 2026-09-06; these findings establish implementation presence, not a new in-game validation.

| System | Existing foundation and entry points |
|---|---|
| Smoke emitters and routing | `engine/Poseidon/World/Effects/SmokeSystem.hpp/.cpp`; includes modern volumes and a legacy plume route |
| Particle motion and rendering | `engine/Poseidon/World/Effects/SmokeVolume.hpp/.cpp`; room tracking, reactive collision, wind/shelter, draw-radius clamping, sorting and approximate self-shadowing |
| World query boundary | `engine/Poseidon/World/Effects/SmokeWorldQuery.hpp/.cpp`; sphere sweep approximation, room lookup, clearance and contained stepping |
| Existing physics service | `engine/Poseidon/World/Physics/PhysicsWorld.hpp/.cpp`, `PhysicsBackend.hpp`, `Box3DBackend.cpp`; Box3D is already linked, with filtered ray queries and registered collision geometry |
| Interior cache | `engine/Poseidon/World/Effects/BuildingInterior.hpp/.cpp`; per-building-type voxel labels and approximate portal records |
| Legacy cloudlets | `engine/Poseidon/Graphics/Rendering/Effects/Smokes.hpp/.cpp`; trace smoke grenades, exhaust, fire and scripted emitters through their actual route |
| Diagnostics | `engine/Poseidon/Dev/Diag/SmokeTab.hpp/.cpp`, retained room/portal overlay |
| Tests | `tests/unit/engine/Poseidon/World/Effects/test_building_interior.cpp`, `tests/unit/engine/Poseidon/Graphics/Rendering/test_smokes.cpp`, `test_cloudlet_depth_contract.cpp` |

Read [the existing rooms/portals handover](handover/SMOKE-rooms-and-portals-20260818.md) as historical evidence. Its early room-detection claims, branch instructions and completion estimates must be revalidated against the current checkout. Its later audit explicitly says that entrance points are not proof of internal rooms and synthetic openings can be wrong. Do not follow historical checkout/deployment instructions blindly or discard newer work.

Important existing behaviour to preserve:

- Interior clearance and contact-plane limits already reduce puff size near walls. Improve them instead of claiming they are missing.
- The modern draw path limits depth pull, especially indoors. Do not restore a whole-radius camera pull that moves smoke in front of walls.
- Cloudlet depth writes are intentionally disabled. Enabling them can punch holes into later geometry and is not a containment fix.
- Room data is also used by rain. Change shared structures compatibly and revalidate rain through openings and roof occlusion.
- Existing wind, fade-in/out, density falloff, fire linkage and self-shadow approximations should be measured before adding equivalent effects.

## 3. Concrete audit targets

1. `InteriorPortal` currently describes a centre, normal and approximate radius. A radius does not precisely describe a rectangular doorway or the remaining opening around a partly open door leaf.
2. `BuildingInterior` is cached per type. Dynamic door state needs per-instance information; two copies of the same house may have opposite door states.
3. The inspected portal/containment code does not expose a live door-animation contract. Find the actual mapping from model selection/animation source to collision transform; do not assume a universal door name or that animation phase is a linear open-area measurement.
4. `IndoorContainStep` currently resolves `BuildingContaining(to)` and returns a permissive default if no object is found. Reproduce whether a long step ending outdoors can bypass the source building's boundary. Treat this as a concrete suspected failure, not a proven runtime bug.
5. A room integer alone is not a globally unique identity. Verify which building owns it before querying clearance or transferring a particle.
6. Current segment traversal has a fixed step cap. Test long displacement and thin walls; a capped sampled traversal must not silently become too sparse to detect a barrier.
7. Existing radius clamps contain deliberate positive bleed/minimum-size allowances. Test their contribution to visible leakage, and the loss of apparent smoke density when particles shrink near a ceiling or door frame.
8. Smoke currently calls legacy `Landscape::ObjectCollision` through its own query adapter, with up to seven rays approximating a sphere sweep. It does not automatically use Box3D because loose objects bounce correctly. Audit the existing physics service before adding more legacy query work or new collision structures.

## 4. Target architecture

### 4.0 Reuse Box3D through the existing query boundary

Box3D is an integrated dependency, confirmed in `vcpkg.json`, `engine/Poseidon/CMakeLists.txt` and `Box3DBackend.cpp`. Prefer reuse of that service for smoke geometry queries when it provides correct coverage and acceptable total cost. Keep `ISmokeWorldQuery` as the consumer boundary and keep Box3D-specific types inside the backend. Read [SIM-804](decisions/SIM-804-physics-integration-boundary-20260831.md), then verify current ownership, update ordering and thread rules rather than assuming its historical caller list is current.

Before selecting the query implementation, establish:

1. Which walls, floors, terrain, glass and moving door leaves are registered near each smoke emitter, including off-camera smoke. A registered convex hull must preserve the intended doorway; a coarse solid building hull cannot answer this query correctly.
2. Which `QueryFilter`/collider kinds provide suitable smoke barriers. Fire/View/Geometry fallbacks have different semantics. Intact glass should block smoke while foliage need not; a bullet-collision mask alone is not a complete smoke material policy.
3. Whether per-instance animated door geometry updates at the time smoke queries run. A rigid object's whole-body transform update does not prove its animated door leaf is represented.
4. Whether the physics world is active and complete in normal gameplay or only populated for particular features/probes. Backend absence or incomplete regional coverage is **unknown**, not a free path.
5. The actual query API needed for smoke. The inspected facade exposes `CastRay`/`RayHitAnything`; it does not expose a complete sphere-cast hit contract. Inspect the installed Box3D API for supported shape casts before planning one. If useful, add a minimal backend-neutral sweep result with hit fraction, point, outward normal, identity and explicit hit/miss/unavailable status. Do not invent an API or treat an invalid body ID as a miss when terrain may have been hit.

Compare the current ray-ring approximation against the Box3D-backed query on the same fixtures. Include world activation, collider registration/update, memory and query costs, not just microseconds per cast in an already warm probe world. A true supported shape cast may be a better replacement; repeated Box3D rays remain an approximation and must not be described as an exact swept sphere.

Retain cached room/portal topology for cheap broad containment and legal opening transitions. Use physics queries for precise local barriers and dynamic leaves where needed. Do not duplicate a second complete collision world just for smoke. A coverage-aware legacy fallback remains available where Box3D cannot answer, with diagnostics explaining the fallback.

Do not create a dynamic rigid body for every smoke puff. Reuse collision detection, then remove inward velocity and let smoke slide/rise rather than bounce elastically. Box3D solves contact against the geometry it knows; it does not by itself prevent the visible billboard footprint from crossing a wall, identify missing room topology, or generate natural smoke flow. Sections 4.2–4.5 remain necessary.

### 4.1 Stable interior identity and static geometry

Keep shared immutable building-type data: conservative solid/free classification, navigable smoke regions and candidate openings. Add stable building-instance identity plus generation/revision to tracked particle location. Model-space queries use the owning instance's transform; unload/destruction invalidates handles safely.

Separate **known solid**, **known free**, **outside** and **unknown**. Unknown or unavailable data must not be reported as a certified open portal. Building path nodes are hints; derive regions with geometry-aware connectivity and inspect actual internal doorways. Use conservative surface voxelisation or equivalent boundary coverage so thin walls are not missed by centre-only occupancy tests.

Do not increase global voxel resolution to fit every door. Keep the broad room representation coarse and handle narrow openings with explicit local aperture geometry. Label counts and grid-size overflow must be handled explicitly, including the existing byte-label representation. Cache builds need a byte cap, bounded build/upload work and a fallback while data is unavailable; first-use geometry work must not stall the frame.

### 4.2 Actual apertures and instance door state

Represent each usable opening with a bounded polygon or rectangle in its wall plane, adjacent region identities and a stable opening ID. A segment crosses a portal only where it intersects that plane inside the aperture and transitions to the correct region. Being near a door centre is insufficient. Support travel both into and out of a room; do not use an outward-direction threshold that accidentally makes doors one-way.

Maintain a small per-instance dynamic overlay for door leaves, shutters and supported breakable openings. Reuse actual collision/animation state where available. Update affected opening bounds/leaf transforms on a state revision, or bounded polling of active nearby doors if no suitable event exists. Keep the doorway itself free in static topology when the geometry supports this, and apply the moving leaf separately; otherwise an open door can remain baked into a solid voxel wall forever.

Closed leaf: blocks smoke. Open leaf: passage follows its remaining aperture. Partly open leaf: use its geometric opening or a conservative tested approximation, not an unrestricted portal plus an opacity fade. A scalar openness may scale visual transport, but must not grant passage through the solid part of the leaf. No blanket assumption that fire/view geometry differences prove glass or a window; supported window state needs evidence. Unknown holes remain diagnostics until validated.

Keep dynamic state shared between simulation containment and render clipping for the same frame/revision. Two building instances must never share mutable door state. Destruction refreshes only affected instance data within a bounded budget; do not rebuild every type or erase valid smoke elsewhere.

### 4.3 Continuous movement and transport

Validate each proposed particle segment against its **source** region, then the earliest crossed boundary, then any legitimate destination. Outdoors-to-indoors movement uses swept broad-phase building candidates, not just an endpoint containment query. For multiple crossings, consume the remaining segment with a fixed limit. If the limit is exhausted, stop at the last verified position rather than allowing the rest of the move unchecked.

Use existing collision/contact response to remove inward normal velocity and permit sliding along walls and ceilings. Smoke should curl and spread, not bounce like rubber balls. Handle spawn-inside-solid and door-closing-on-particle cases with bounded projection to a valid side or controlled dissipation; do not teleport the puff through the leaf. Closing a door stops new transfer, but smoke already on both sides remains there and decays normally.

Puffs are samples of smoke, not rigid spheres that must fit through a doorway at their full outdoor size. Near a valid aperture, permit a smaller local puff footprint and gradual expansion after crossing, or bounded splitting from the existing particle pool if it is demonstrably needed. Preserve approximate total visual density when resampling; splitting must not multiply smoke or increase the global cap. Test flow through a doorway narrower than the nominal puff diameter.

Natural passage should arise first from emitter momentum, rising motion and existing local air motion. If particles pile up despite an open route, use a modest bounded aperture-directed drift or coarse room-fill exchange. Label this an artistic transport approximation, not pressure physics. Do not instantly copy smoke into every connected room, force it through closed doors, or make the effect depend on camera location.

### 4.4 Visible containment

Start by correcting true depth and existing clearance/contact limits, then demonstrate what remains. For difficult near-wall and doorway cases, choose the cheapest representation that can express the required boundary:

- Clip each puff against a small bounded set of nearby wall/door planes when a locally convex region is sufficient.
- Where concave rooms or apertures make a few planes insufficient, evaluate a cached local containment mask/field for the puff's actual building/region, or partition the puff into supported local pieces within the same budget.

Choose one measured path after a fixture comparison, not both by default. Specify how world position is reconstructed for billboard fragments, how interpolation and model transforms work, and how a puff spanning an open aperture is permitted on both valid sides. Testing only a centre's room label in the shader would recreate the original bug. A whole-room stencil would also be insufficient if it fills across windows or disconnected rooms.

Clip planes/masks must forbid solid-side coverage with any smoothing feather directed into free space. Do not hide a wall leak by enlarging the aperture or keeping an arbitrary bleed distance. Provide a conservative clipped/smaller-puff fallback when plane/mask capacity is exceeded. Draw bounds must cover all valid particles so a roomful of smoke does not vanish when the emitter/volume centre leaves the view.

Normal opaque depth testing remains required. Optional soft-particle depth fade smooths contact with foreground surfaces; it cannot replace world-space containment. Respect reversed-Z, MSAA depth conventions, the transparent ordering around water/clouds, and separate weapon-depth segments. Smoke must not write ordinary scene depth. Inspect the actual billboard renderer before assuming the generic 3D shader is the correct integration point.

### 4.5 Appearance at similar cost

After containment passes, tune existing systems for less obvious billboards: restrained variation in size/rotation/lifetime, smoother age transitions, slower coherent turbulence and ceiling spreading. Use shared atlas/normal data and cached room/local illumination where supported. Preserve self-shadowing and assess its spatial-hash cost before extending it; no per-particle shadow maps or many world rays per puff per frame.

Keep opacity/transmittance consistent when particles are resized, culled or reduced by LOD. Avoid denser smoke appearing when quality is lowered or a frame-rate change alters spawn count. Use correct straight/premultiplied alpha conventions for the actual path. Check exposure, dark interiors, sunlit doorways and fire-lit smoke without introducing self-luminous grey clouds.

Only experiment with half-resolution smoke if fill rate is the measured bottleneck. Depth-aware upsampling must preserve thin door frames and walls; include target allocation, compositing and depth-reduction costs. Keep this optional if it blurs smoke over boundaries. No resolution change is needed to ship the containment fixes.

## 5. Performance and correctness contract

Targets are proposed, not measured results. Freeze a representative baseline in SMC-001 and record hardware, settings and actual render resolution.

- Aim for neutral frame time; cumulative median CPU and GPU frame-time regression must each stay within 5% against the original baseline in affected scenes. Establish absolute millisecond caps as well and apply the tighter limit.
- Investigate repeatable p95 regressions, first-use cache-build stalls and door-animation spikes. Measure alternating baseline/candidate runs with at least three matched rounds, warm-up and sufficient steady-state frames; uncertainty is not a pass.
- Bound global particles, active instances, grid/cache bytes, dynamic opening updates, per-particle boundary work and render-mask/plane resources before implementation rollout. Do not allocate a simulation volume per room.
- No rebuild per particle or per frame, no synchronous GPU readback, no unbounded ray/triangle loop and no world-wide full-resolution fluid grid.
- Count both query work and rendered overdraw. Sum only non-overlapping timing regions. Record cold-cache and steady-state costs separately.
- Lower quality reduces cosmetic turbulence, shading or particle detail, not solid-wall correctness. On exhausted collision work, retain the last verified position or safely dissipate; never skip containment and integrate unchecked.
- A positive doorway transfer test is mandatory: zero wall crossings achieved by freezing or deleting all smoke is a failure.

## 6. Agent work packages

### SMC-001 — Baseline and routing audit

Trace a grenade, fire plume, vehicle exhaust and scripted emitter where available. Record which use modern/legacy paths and actual containment settings. Establish named indoor/outdoor captures, moving cameras, smoke counts, CPU simulation/query/draw time, GPU smoke cost and memory. Trace animated door transforms and shared rain consumers. Complete the Box3D coverage/API/cost audit in section 4.0 and record the chosen query service with evidence. Add missing diagnostics only where necessary.

Exit: source-route table; named reproducible failure; exact door-state source or recorded missing-data fallback; budget table; no assumption that the modern smoke test covers all gameplay smoke.

### SMC-002 — Identity and continuous containment

Dependencies: SMC-001.

Fix source-instance identity, stale handles and continuous traversal before artistic changes. Integrate the Box3D query adapter if SMC-001 establishes its suitability; otherwise document the concrete missing capability and retain the verified fallback. Add full-build geometry fixtures rather than only pre-labelled table tests. Reproduce endpoint-outside movement, thin walls/roofs, long segments, adjacent buildings and multi-storey cases. Correct conservative occupancy without sealing legitimate openings.

Exit: no invalid solid crossing in the deterministic fixtures, bounded segment work, stable building ownership, and a valid open passage still traversable. Compare rain regressions after shared changes.

### SMC-003 — Dynamic doors and precise apertures

Dependencies: SMC-002.

Implement a single real animated doorway end to end, including static-cache separation and per-instance leaf state. Test closed/open/partly-open, both directions, repeated toggles, closure during crossing, two identical houses with different states, and opening after smoke has accumulated. Then extend to internal doors and outdoor-to-indoor entry. Keep window/breach inference conservative.

Exit: no passage through the closed leaf or frame, observable transfer through the open aperture, no stale cache, and bounded update cost. Record unsupported imported door families individually.

### SMC-004 — Puff footprint containment

Dependencies: SMC-003.

Keep movement fixed while testing render changes. Start with current depth/clearance improvements, then select bounded clipping planes or a local mask only if needed. Include large puffs beside a thin wall, concave room corners, a narrow doorway and a ceiling. Check from both sides at grazing angles and while standing in the smoke. Account for clipped-puff density and aperture-local resampling.

Exit: no visible wall-side smoke beyond a recorded antialiasing tolerance in controlled fixtures; smoke remains visible through the opening and fills the room naturally. Normal depth order and frame cost pass.

### SMC-005 — Visual polish and emitter integration

Dependencies: SMC-004.

Tune rising/spreading motion, fade, texture variation and lighting in matched scenes. Add coarse aperture drift only if the movement evidence warrants it. Route supported gameplay smoke sources through the verified containment path without duplicating emissions or changing duration/colour semantics. Do not apply smoke behaviour to sparks, flame sprites or every generic particle automatically.

Exit: visibly improved motion and shading; grenade/fire/scripted source coverage explicitly recorded; outdoor plumes and exhaust retain their intended appearance; no increases hidden by particle deletion.

### SMC-006 — Stress, compatibility and release gate

Dependencies: SMC-002 through SMC-005.

Run the full matrix, rain regression and extended traversal. Check cache eviction, unload/reload, destruction and supported temporal reconstruction modes. Validate frame-rate-independent emission/motion within existing simulation design, persistent containment when off-screen, and smoke viewed through an open door from another room. Record known unsupported geometry; do not promise arbitrary broken collision meshes are watertight.

Exit: cumulative cost contract and visual gates pass with evidence. Update the appropriate renderer/execution record without asserting unperformed owner review. Implemented and owner-accepted are separate states.

## 7. Validation matrix

| Fixture | Required result |
|---|---|
| Sealed room, thin walls and roof | Smoke spreads inside; none transfers through solid geometry |
| Open exterior doorway | Smoke crosses actual opening both ways under suitable motion; frame remains solid |
| Closed and partly open door | Actual leaf blocks passage; remaining gap behaves conservatively |
| Two rooms and internal door | Correct region transition; no transfer across the neighbouring wall |
| Door closes while smoky | New transfer stops; existing smoke on either side does not disappear |
| Door opens after accumulation | Smoke can begin leaving without restarting the emitter |
| Two copies of a building | Independent door states and room identities |
| Large puff/narrow opening | Door passage works without rigid-sphere blockage or wall overlap |
| Camera outside/inside/grazing | Same world containment; no plume-centre culling or depth-pull leak |
| Fast step and spawn in solid | Bounded safe response, no tunnelling or cross-wall teleport |
| Multi-storey, concave room | No floor/ceiling leak; no single-plane assumption hiding the other room |
| Glass/window/breach | Only validated open geometry passes smoke; missing metadata is reported |
| Water, foliage, weapons, fire | Correct transparency/depth order and stable lighting |
| Heavy smoke, several buildings | Global budgets hold; fill rate, queries and first-use costs measured |
| Rain and streaming | Shared interior changes preserve rain behaviour and lifetime safety |
| Physics query coverage | Box3D present/absent/partially populated; terrain hit versus miss; animated leaf updates; correct barrier filters and legacy fallback |

Keep counters for blocked solid crossings, accepted aperture crossings, unknown-data fallbacks, stale handles, traversal-limit hits, door updates, active/cache bytes and particle/draw counts. Test coverage must include at least one positive aperture crossing; counters alone do not establish visible containment. Use labelled before/after sequences or repeatable motion, not a single convenient still.

## 8. Verification, evidence and handoff

Use [Windows build documentation](../build/win.md), [test documentation](../../tests/README.md) and the installed-game instructions. Verify current runner options before use. Deploy matching executable and renderer DLL for runtime evidence, subject to normal tool permissions. A generic startup smoke test is not a smoke-particle containment test.

Reuse `POSEIDON_TEST_SMOKE_HOUSE` and the room overlay only after confirming their active call sites and selected building. Add named deterministic door fixtures where the existing nearest-building hook cannot reproduce the exact case. Preserve the current deployed-build provenance process; do not copy an old branch command from the historical handover.

Store evidence in `docs/roadmap/evidence/` under a unique SMC stage/date prefix. Each handoff records baseline/candidate fingerprints, exact map/building/camera/door state, commands, particle/settings counts, test results, captures, CPU/GPU median/p95, memory, limitations, rollback and next dependency-ready package.

These SMC IDs are local to this plan. Do not append them to [status-ledger.yaml](status-ledger.yaml) as authorised Preview-0 tickets. Inspect the active scope convention and [renderer-systems-ledger.yaml](renderer-systems-ledger.yaml) before recording implementation. Update the older smoke handover with a link to this plan when implementation supersedes its instructions; do not erase historical evidence.

If metadata is inadequate, retain the existing reactive fallback and mark that building's containment guarantee unsupported. A bounded geometry-query fallback may improve it, but a missing collision wall cannot be fixed by a shader parameter. Continue independent supported work and present the exact remaining asset/visual blocker. Completion means smoke respects verified solid boundaries, still passes open doors, looks better in motion, and meets measured resource limits.

## 9. Implementation status (2026-09-07)

**The plan's premise needed correcting before any of it could pay off.** An audit of the
routing (section 3, item 1) found that NOTHING in gameplay reaches `SmokeVolume`, where all
the containment work lives: `SmokeSystem.hpp:24` says so in as many words ("Both spawn paths
are dev-driven"). Every smoke the player sees is a LEGACY cloudlet -- smoke grenades
(`SmokeShell` holds a `SmokeSource`), vehicle exhaust and wheel dust, burning wrecks,
explosion fire and craters, muzzle clouds -- and `Cloudlet::Simulate` was pure ballistics
with no collision, no room and no world query of any kind. So the walls were never leaking:
gameplay smoke had no containment to leak through.

**SMK-030 (done):** legacy cloudlets are contained.
- Every cloudlet asks `IndoorRoomAt` ONCE, at its first step. Outdoor smoke -- exhaust,
  dust, artillery, every plume in the open -- never asks again, which is what keeps this
  affordable.
- A cloudlet that started in a room is stepped with `IndoorContainStep` (the cached
  per-building voxel walk, not a geometry sweep). Blocked steps lose the velocity INTO the
  wall and keep a slice along it, so smoke slides and curls instead of stopping dead;
  legitimate room changes through open doorways are followed, and a cloudlet that leaves the
  building is outdoors and free from then on.
- Switchable: Smoke tab "Game smoke stays in its room (SMK-030)" plus wall-slide slider,
  `POSEIDON_SMOKE_CONTAIN_LEGACY=0` / `POSEIDON_SMOKE_CONTAIN_SLIDE`. Counters (asked /
  indoors / steps / blocked) are on the tab and in the load log.
- Measured, smoke grenade in the Everon cafe, same pose and seed A/B: cloudlet simulation
  0.11-0.18 ms with containment against 0.07-0.15 ms without, i.e. **+0.03 ms with 730
  indoor particles**; whole-frame 6.28 ms on against 6.52 ms off (noise). 1,574 spawn
  queries and 11,361 blocked steps over the run. Zero wgpu errors. The room fills with
  smoke instead of draining through the walls.

**Not done, and what the plan should be read as next:**
- Outdoors-to-indoors is still free: a plume outside a house can drift in. The one-way case
  (smoke made inside stays inside) is the one the owner reported; the other direction needs
  the swept broad-phase of section 4.3.
- Rendered footprint (section 4.4) is untouched for cloudlets: a big billboard next to a
  wall can still paint through it. `SmokeVolume::Draw` already has the clearance and
  depth-pull clamps to copy; that is the next cheapest win.
- Dynamic door leaves (SMC-003) and precise apertures remain as written; containment
  quality is still capped by the building's fire-geometry quality.

