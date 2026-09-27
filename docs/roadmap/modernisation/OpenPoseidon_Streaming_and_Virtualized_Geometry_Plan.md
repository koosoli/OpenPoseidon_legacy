> Execution note, 20 September 2026: work proceeds in `codex/streaming-residency`,
> based on verified `03e713714b9b699e3f6f4eae7cdce7f488a13b64` in the existing
> `combat-audit` worktree. The historical branch examples below are not switch targets.
> Waves are parked at the owner's request; `safety/codex/backup-waves-20260920`
> preserves that baseline. The old ocean remains the default.
> This is a staged plan, not a completion claim. Pressure-driven object eviction and
> geometry paging remain gated on gameplay ownership and measured performance evidence.

# OpenPoseidon: shared asset streaming and optional virtualized geometry

## Agent work order and technical assessment

Prepared: 19 September 2026  
Revision: **4 — WGPU-focused development; removes all GL33 improvement/porting work following the owner's retirement direction. Supersedes revisions 1–3.**  
Repository reference: `koosoli/PoseidonWGPU-private-safety-mirror` — verify the actual checkout/remotes before working.  
Implementation baseline: **automatically detect and verify the active development branch and exact HEAD at task start; do not hard-code a branch from this document.**  
Latest owner-reported branch: `codex/squad-ai` — a discovery hint, not a permanent target or proof of the active local worktree.  
Historical source-review branch: `parity/foliage-lighting-001` — evidence provenance only, NOT an instruction to check it out.  
Historical source-review commit: `e486e7191579ea9d3e5a0c0c09d34f417c944f80`

This document proposes work; it does not report implementation or new runtime tests. The original streaming observations in Sections 2.1–2.4 and 3 refer to the historical source-review commit, not automatically to the branch active when an agent starts. The targeted terrain/water addendum in Section 2.5 and Workstream T was separately checked against the remote `codex/squad-ai` branch at `f08ff0d7674e066944f0cf6195ae39e2e94f48aa` on 19 September 2026; this is limited source provenance, not a full re-audit or a required implementation baseline. Historical measurements embedded in source comments are developer reports, not independently reproduced results. Discover the current baseline using Section 0 and revalidate relevant findings before changing code.

## 0. Required first step — discover the current branch and pin the working baseline

Inspect the actual repository and task worktree before reading this plan as implementation instructions. Use read-only Git inspection such as:

```sh
git rev-parse --show-toplevel
git status --short --branch
git branch --show-current
git rev-parse HEAD
git worktree list --porcelain
git branch -vv
git remote -v
```

Redact credentials from remote URLs and reports. Read the applicable repository instructions and current handovers to establish which worktree/branch is the active development line for this task. The owner most recently reported `codex/squad-ai`, but it may have changed. Neither that name, `parity/foliage-lighting-001`, the repository default branch, nor the branch with the newest timestamp is sufficient evidence by itself.

Record the verified repository, worktree, active branch, exact commit SHA, upstream relationship and working-tree status. If remote freshness matters, fetch only the relevant refs without changing the checkout; record divergence instead of automatically pulling, rebasing or resetting. A remote branch listing alone cannot establish which local branch the owner is working on. If operating only through remote tools, state that limitation and use the task's branch/worktree metadata and latest owner reference without claiming local verification.

If HEAD is detached, identify its intended base from the task metadata, worktree information and current handovers. If the intended base remains unresolved, keep the investigation read-only at the recorded SHA and report that uncertainty; do not guess a branch and start implementation there.

Compare the relevant streaming/renderer code at the verified baseline with the historical review. Classify each proposed issue as still present, changed, already fixed or unverified. Follow current definitions and call sites, since file names, line numbers, defaults and ownership may have changed. Do not reimplement fixes already present. Keep the source links in Section 7 pinned as historical evidence rather than relabeling them as a review of the new branch.

Create any isolated feature branch/worktree from the **verified current committed baseline**, not from the historical review commit. Do not switch or rewrite another agent's checkout, overwrite an occupied branch, or discard/stash/commit somebody else's changes. If relevant uncommitted work exists, record that it is not included in a branch created from HEAD and account for ownership before overlapping edits. Do not silently claim that an isolated worktree contains those changes.

Pin the chosen SHA for the investigation and its A/B tests. Recheck the active integration branch and relevant incoming changes before handing off; do not silently change baselines midway through measurements. The objective is to improve the current engine, not to return it to the state in which this document was originally written.

## 0.1 Renderer scope — no new investment in GL33

**Owner direction, 19 September 2026: GL33 is approaching retirement and does not need improvement. WGPU is the sole rendering-development target of this plan.** This direction overrides the optional GL33 work in earlier revisions. Revision 4 changes the work order, not the source-review baseline; it does not claim a new code audit or implementation.

Do not implement GL33 terrain/water instancing, port CWR-CE #314 into GL33, refactor its shader sources, add a new GL33 test programme, optimize its performance or bring it to feature parity. Do not maintain parallel versions of new rendering features solely for GL33. Its constraints must not drive WGPU architecture, resource layouts, feature selection or scheduling.

While the existing repository still includes GL33, limit attention to the smallest necessary compatibility checks or glue repairs for shared-code changes made by this task, using existing build/tests where applicable. Do not turn those checks into backend modernization or a feature-parity gate. If legacy compatibility would require substantial extra work, record the conflict for the integration/retirement decision instead of expanding this work order. Removing GL33 itself is also outside this task; do not perform an unrequested backend deletion or CI cleanup.

Shared asset and simulation services must still work without a renderer for headless/dedicated-server execution. That is a simulation/architecture requirement, not a justification for extending GL33. Preserve multiplayer, save/load, logical state and collision correctness independently of graphical features.

In this document, new rendering **fallbacks** mean supported WGPU alternatives: conventional rendering when geometry paging or optional GPU features are unavailable, resident coarse geometry/mips while detail loads, or the approved WGPU path when an experiment is disabled. They do not require implementing the feature in GL33 or automatically switching the engine to GL33. Capability checks for supported WGPU devices remain necessary.

## 1. Decision

Improve the existing shared streaming infrastructure. Do not build another Reforger-specific streamer and do not begin by attempting to clone Nanite.

The native Reforger loader already connects archive-backed models and spatial placement metadata to the core modern-object streamer. Background model preparation, texture preparation, predictive camera loading, caching, GPU culling, conventional LOD selection, texture eviction and GPU mip-demand instrumentation already exist. Their coverage, lifecycle contracts and pressure response are the questions to resolve.

Recommended order:

1. Establish an accurate active-path and ownership baseline, including simulation correctness.
2. Reproduce and repair residency invalidation/pressure-response gaps with the existing experimental controls kept isolated.
3. Improve measured main-thread preparation/upload bottlenecks and memory accounting without duplicating existing workers or caches.
4. Build a conservative texture-detail controller using existing feedback, with valid coarse fallbacks and bounded allocations.
5. Reuse and improve conventional GPU LOD/culling and far representations.
6. Only when measurements justify it, prototype independent hierarchical geometry paging for a few static opaque assets.

The desired result is smoother traversal and predictable resource use at comparable visual coverage, not simply a lower object count or blurrier textures. Rendering improvements must not change AI, collision, projectile obstruction, navigation, mission object identity, destruction persistence or multiplayer authority merely because a camera moved.

Include the small active-path check from Workstream T in the baseline. CWR-CE #314 is an optional rendering reference, not a new streaming dependency or a mandatory port. In the addendum review, `Landscape::DrawWater()` already delegates to `IWaterRenderer` and returns before the legacy water meshes. Protect that routing and measure any remaining cost; do not schedule an already-present bypass as a new optimization. Keep the existing WGPU CDLOD/modern-water architecture. Only investigate ideas that could improve the current WGPU path or shared asset services; GL33 instancing and GL33 shader improvements are out of scope under Section 0.1. [A1, U1–U2]

## 2. Existing implementation: verify, reuse, do not reinvent

### 2.1 Native Reforger route

`World/Terrain/LandLoadEnfusion.cpp` contains the native `.pak` world-loading path. In the reviewed implementation, the `nativeStreaming` setting is enabled by default. It builds compact placement records and a model table using `.xob` resource paths, bins placements into land cells, and enables `_modernObjectStreaming`. Attached building parts are included as placements with composed transforms. `EnsureModernObjectPreparer()` arms background work. [R1–R3]

The central path is approximately:

```text
Mounted Reforger .pak resources
  -> world/prefab/material resolution and terrain loading
  -> XOB model names + compact placement transforms + cell index
  -> Landscape::UpdateModernObjectResidency
  -> shared model preparation / shape installation
  -> object creation + renderer registration
  -> retained GPU rendering and texture residency
```

Native loading is not equivalent to “everything is lazy.” The reviewed loader resolves substantial world metadata at load; its attachment-mesh build loop is unconditional even when main meshes are deferred. It also builds road shapes and adds road objects separately. Determine the actual startup costs and ownership of those exceptions before changing them. Do not remove an eager path merely because its loop looks suspicious: establish all its consumers. [R1–R3]

`POSEIDON_REFORGER_STREAM=0` selects the diagnostic nonstreamed/radius-at-load alternative. That alternative must not be mistaken for the current default. Native support here means OpenPoseidon's loader; this review does not claim to describe Bohemia's proprietary runtime scheduler.

### 2.2 Shared modern route versus legacy worlds

`LandSave.cpp`'s modern OPRW loading path defaults to cell streaming unless `WGR_OBJECT_STREAMING=0`. Its object budget defaults to 20,000 placements. It has candidate caching, stable distance sorting, window growth, camera-velocity prefetch, a released-shape cache, stale-work cancellation and admission controls. [R4–R7]

By contrast, the inspected legacy `Landscape::LoadData()` path loops over and creates all world objects. Do not conclude that the original OFP/CWA world-object path already has the same spatial lifecycle. Also do not conclude that it has no asset management: file-backed WGPU textures are lazy and shared texture/model infrastructure exists separately. [R4, R11]

Build an explicit matrix for legacy OFP/CWA, modern OPRW, native Reforger, loose files, PBO members and PAK members. For each, identify active reading, decoding, material resolution, CPU retention, GPU upload, eviction and reload paths. Verify headless/server behavior independently; a WGPU terrain-draw call is not a simulation-safe scheduler. GL33 requires no expanded path audit or new streaming implementation; apply only the limited shared-code compatibility policy in Section 0.1.

### 2.3 Background work already goes beyond file reads

`ObjectStreamPrepare.cpp` first tries the safe loose-file loader. For archive-backed XOBs it can invoke the registered external native loader on a worker. The native texture-preparation branch handles relevant DDS/EDDS and material texture names, including some composites. Do not repeat older comments claiming all native models or all native textures are main-thread-only. [R8]

`ObjectStreamPrepare.hpp` and the admission path expose parse/ready/conversion/converted states. Bank-table resolution, proxy completion, shape-bank mutation and final installation still have explicit ownership boundaries. Compatible adapter work is submitted to workers. Trace source-format eligibility rather than assuming every XOB or P3D variant takes the same conversion route. [R6, R8]

The model queue already has request deduplication, generation cancellation and count limits. `PreparedTextureStore` already has a byte budget, TTL, generation checks, decoded/blocked/prepared DDS handoffs and uploaded/evicted tracking. Its stated default retained-byte budget is 512 MiB. Extend accounting where missing; do not introduce another parallel preparation cache. [R8–R9]

### 2.4 Rendering and texture infrastructure already exists

The GPU culling representation in `rust/src/gfx3d/cull.rs` is instance -> model -> drawable LOD -> section, with indirect argument construction and Hi-Z-related inputs. It is not evidence of a Nanite-like hierarchical geometry-page system. Establish which source assets/material classes actually use the retained path, and the fallback cost for the rest. [R12]

`TextureWgpu` has lazy upload, GPU eviction, stable bindless-slot leases and residency pins. `TextureBankWgpu` has an object-texture budget share and LRU trimming. Its `UseMipmap` interface currently calls `EnsureUploaded` rather than directly implementing requested per-mip residency. [R10–R11]

`gfx3d/mip_feedback.rs` already gathers sampled mip demand through a rotating slot set and asynchronous readback. `TextureBankWgpu::ReportMipFeedback` explicitly treats this as measurement, not an adaptive detail policy. Confirm all current consumers before extending it. A global upload-time mip bias is not the same feature as independently promoting and demoting each texture as demand changes. [R10–R13]

`ModelDerivedCache` already connects P3D-to-model-IR production to the derived-blob store. Reuse this infrastructure when additional cached production genuinely pays. Verify native XOB/material dependencies separately rather than assuming the P3D key is sufficient. [R14]

### 2.5 CWR-CE #314: WGPU-relevant ideas only, not a porting task

Upstream reference: **“Porting instanced terrain (water) rendering,” CWR-CE PR #314**. At the 19 September 2026 check it was open/unmerged, comparing base `ce7c153746d659359260934c5470e46650e81f83` with head `6469b7f289e6620aae48ca367d15280976849117`. Only if a specific idea merits WGPU/shared-service reuse, re-fetch its status, head/base and review feedback before that reuse; this is not a request to port the GL33 implementation. GitHub's temporary mergeability commit is not evidence that it has merged. [U1]

The proposal uses a reusable mesh for an 8×8-cell segment and submits `GroundSegment` records rather than individual land-cell instances. It has persistent per-cell texture/batch metadata, per-segment batch membership and water-presence flags, and a GL33 instanced water path. Distinguish these representations: the inspected `cellInfoTex` carries texture/batch/simple/valid metadata, while `segHasWater` is a CPU-side segment-filtering structure—not proof of GPU per-cell water-mask streaming. [U2]

Reducing instance records is not a proportional FPS gain. The mesh still contains the constituent cells, and a segment containing several texture-array classes can be submitted once per relevant class. For any idea applied to WGPU, measure repeated vertex work, CPU submission, draw calls and memory independently. Do not create a GL33 benchmarking/optimization project. This PR is not asset streaming, virtual texturing or hierarchical geometry paging. [U2]

**Correction to the preliminary conversational assessment:** the presence of `DrawWater(bigRect, scene)` in `DrawRect()` does not show that WGPU executes the old CPU water path. At OpenPoseidon commit `f08ff0d7674e066944f0cf6195ae39e2e94f48aa`, `Landscape::DrawWater()` begins with an `IWaterRenderer` dispatch and an early return; the `_segCache.Segment` / `_wTable` loop is below it. When that backend interface is present, this call does not fall through into those legacy meshes. Deleting the wrapper call would remove the modern water submission, not optimize a proven duplicate. [A1]

Source confirmation is conditional on the backend providing the interface; verify creation, configuration and fallback paths in the active build. The surviving question is whether other callers generate avoidable segments or duplicate preparation—not whether this already-protected branch needs the same protection again. Segment work legitimately used for terrain, grass or another pass must be attributed to that consumer.

Preserve OpenPoseidon's existing `ITerrainRenderer` / `IWaterRenderer` integration and WGPU CDLOD architecture. Inspect the current WGPU implementation before choosing changes. Do not import upstream shared-`Engine` signatures or a GL33-specific global switch merely to make its patch fit. The upstream GL33 shader refactor and tests are not implementation tasks for this plan. Reuse existing WGPU shader-composition, validation and ABI tests for any actual WGPU changes. A clean test run is not proof of runtime visual correctness. [A1]

## 3. Concrete priority issues

### 3.1 World-object lifetime is currently coupled to camera residency

The inspected eviction loop clears `placement.resident`, calls `RemoveObject`, erases the ID-map entry and drops light-owner state. It retains the model shape, not the complete live object. A native far-render hook and an observed-destruction bit exist, but that is not proof that all gameplay state is retained. [R7]

This is a correctness risk to investigate, not a claim that every scenario already fails. Trace `RemoveObject`, object lookup/recreation, simulation spatial indices, collision/view/fire geometry, geography accumulation, destruction and save/load. Test whether moving the spectator camera can change a distant unit's physical world.

Before generalizing to original OFP maps, define at least three independent needs:

- Logical identity/state: stable placement identity, damage, animation/door state, scripts and authoritative mutations.
- Simulation/query residency: collision, fire/view geometry, support surfaces and navigation needed by active simulation or queries.
- Visual residency: render instances, geometry LODs, materials and texture detail needed by views and dependent passes.

Reuse existing state facilities where possible. A compact identifier is not a substitute for saved mutable state. A far visual proxy is not a collision proxy.

For an initial legacy-world improvement, prefer streaming visual resources while leaving the current logical/collision lifecycle intact. Do not switch all legacy placements to camera-driven destruction as a shortcut to lower RAM use.

Define actual simulation interests: authoritative actors, remote AI, vehicles, active projectile/query corridors, scripts and multiplayer ownership—not only the player's camera. A radius around the player alone cannot establish correctness for all long-range queries. Where safe asynchronous query completion cannot be integrated, retain required data or provide the existing correct fallback. “Not resident” must never silently mean “no obstacle.”

### 3.2 Experimental GPU object budgeting has both a policy problem and an invalidation problem

`GpuClampedObjectBudget` is opt-in (`WGR_OBJECT_STREAM_GPU_BUDGET=1`). Its source records oscillation under constrained memory and an unresolved relationship between desired-count reduction and actual releases. The current estimator uses object-attributable tracked bytes divided by resident object count; those objects can share meshes and textures, so this is not a marginal eviction-cost model. [R5]

There is also a specific control-flow hypothesis to reproduce:

- The residency update early-returns when the center has not changed and no work is pending, before evaluating the object-budget controller.
- Desired-cell selection and cell release are guarded by `centerChanged`.
- Candidate caching is keyed to the center, although candidate-window growth uses the effective budget.

Therefore a budget change need not immediately rebuild the desired set or evict anything while the camera is stationary. Check all callers/other invalidations and create a failing test before changing this. Do not address only the controller's smoothing while leaving its decisions unable to reach the release path. [R5–R7]

Separate dirty reasons such as center, demand, pressure/budget, world generation and simulation pins. Coalesce them and avoid recomputing a full world sort every frame. Preserve the fast settled path.

### 3.3 Nonblocking preparation does not imply bounded frame latency

The existing admission code has count, byte and time controls, including native bank-table throttling. It still has indivisible work items and fallback paths that can cost more than an intended frame budget. Counted admission and timed admission are different modes: report which bound is active. Do not present a disabled threshold's zero counter as evidence it works. [R6]

Measure parse, material/header work, adapter conversion, proxy completion, installation, texture upload, buffer growth and GPU retirement separately. If a single task is too large, split the task or prepare it earlier; changing “objects per update” alone cannot bound it. Do not wait for a worker or GPU readback from the critical frame path.

### 3.4 Memory accounting must describe allocations and shared ownership

Count limits remain useful emergency limits, but resource pressure must be evaluated with resource bytes and ownership. Track distinct shared geometry/texture allocations once, separate from placement counts. Report CPU source/decoded/compiled/ready storage, GPU live capacity, staging, pending replacements and retired allocations not yet reusable.

The texture-preparation store is already byte-bounded; do not count it as an unbounded queue. Audit model parse/conversion queues and transient peak working memory separately. A retained-store cap does not automatically cap simultaneous decodes before publication. [R8–R9]

Driver budgets, where available, are not identical to renderer payload counters. Query the active implementation and device limits; do not infer total VRAM from maximum buffer size or assume a historical 2 GiB buffer ceiling is the current problem. Reserved heap capacity and submitted resources awaiting retirement are not free memory.

## 4. Staged implementation

### Stage A — Baseline, ownership audit and minimal reproduction

Complete Section 0 first. Treat the historical findings as leads to revalidate against the automatically verified current branch, not as a current bug list. Include the verified branch/SHA and any already-fixed items in the baseline report.

Use existing `StreamingDiag` snapshots, Streaming tab, capture metrics, Perfetto, memory counters, model timings and mip-feedback reporting. Add only missing causal measurements. [R15]

Produce:

1. Active-path matrix and source-referenced ownership diagram.
2. Cold-start, warm-start, forward traversal, reversal and stationary-pressure captures at a pinned build and content configuration.
3. A read-only camera-independence test: run simulation in one area while moving a remote/spectator camera elsewhere; compare collision, visibility, IDs and persistent mutations.
4. A test of object-budget changes without camera motion, including a change during an unfinished fill and a change after convergence.
5. Attribution of the largest observed stalls, not only aggregate FPS.
6. The bounded Workstream T routing check: identify the actual terrain/water owner, verify the existing modern-water early return, and attribute any legacy segment generation by caller/pass. A confirmed absence of redundant work closes that hypothesis without a speculative patch.

Use normal supported settings as the main comparison. A diagnostic eager path or deliberately tiny budget may isolate a cause but is not the reference for headline gains. Fixing texture preparation cannot be credited for unrelated terrain or simulation cost. Likewise, upstream GL33 results cannot be credited as WGPU streaming gains. Keep the original Stages B–G priorities. No GL33 port, optimization, shader refactor or new GL33 validation work is scheduled.

### Stage B — First code slice: controlled residency invalidation

If the stationary-budget hypothesis reproduces, fix the minimal control-flow/invalidation issue in a focused change. Keep the unstable pressure controller opt-in. This is not authorization to enable it globally.

Required tests:

- Center unchanged, no pending work, explicit budget/demand dirty reason: reevaluation occurs.
- Budget shrinks/grows: desired set and actual releases react correctly without requiring camera movement.
- No dirty reason: preserve the fast early return and avoid a fresh world scan.
- Existing pins/required identities are respected; no partial building dependency is invalidated.
- Repeated pressure signals coalesce; stale generation results cannot repopulate a departed world.
- Feature disabled: current approved behavior retained.

This first slice should improve controllability and observability without inventing a universal resource scheduler.

### Stage C — Stable pressure response and bounded preparation

Build on resource accounting and the corrected invalidation path. Use high/low water marks, a decision dwell period, stable priorities and bounded growth. These are aids to stability, not substitutes for knowing whether an eviction releases any allocation.

Prefer dropping expendable caches and fine visual detail over deleting near-field required objects. Rank candidates using reclaimable bytes, reload cost, age, visual need and dependencies. Avoid double-counting a mesh referenced by thousands of instances. Deduct fixed render resources and temporary/retired allocations from the appropriate budget before promising an object pool more room.

When mandatory state exceeds a soft budget, stop speculative work and report the overage. Do not silently erase collision, dereference invalid state, or loop endlessly trying to reclaim pinned memory.

Extend model-preparation admission with byte reservations and bounded temporary allocations where absent. Handle unknown or oversize assets explicitly; reserve capacity for one useful task so a byte gate cannot deadlock progress. Bound task concurrency and check cancellation between expensive stages. Never relax validated bank/main-thread ownership just to make a job asynchronous.

Under repeated traversal and stationary pressure, measure actual byte convergence, cache churn and reloads. The controller must settle; a lower instantaneous object count is not acceptance.

### Stage D — Close only the measured synchronous-path gaps

Inventory preparation hits/misses by source and texture role. Native XOB and some DDS/EDDS preparation already work; extend uncovered expensive dependencies rather than duplicating them. Investigate unconditional attachment prebuilds and road preparation only when load-time data implicates them.

For PBO-backed resources, preserve mount precedence, decompression semantics and ownership. Do not call the current unlocked file-server MRU from arbitrary workers. Options include an independently owned read path with immutable archive indexing or an owned I/O service feeding pure decode jobs. Choose the smallest tested solution; no whole-file-server rewrite without demonstrated benefit.

Keep renderer uploads within the established producer/render-thread protocol. Use bounded staging, bytes and task costs. Progress means completing useful work or presenting an already valid coarse representation, not being forced to execute one enormous synchronous load per frame.

Preserve assembly consistency: a building and essential doors/openings should not be accidentally starved by an independent count cap. Prioritize dependencies and use a valid complete coarse representation while detail is unavailable. Do not label every missing model “cosmetic.”

### Stage E — Adaptive texture detail using existing feedback

First verify feedback coverage, age, slot identity and all existing consumers. Then prototype selected file-backed object textures, not UI/fonts or every terrain texture at once.

Keep a coarse mip tail available, request finer detail using both conservative predicted demand and fresh GPU observations, promote promptly and demote slowly. Use the finest demand across relevant views/passes; no report from a rotating feedback group means “not yet observed,” not permission to evict. Distinguish unsupported feedback, stale samples and genuine low demand.

Prevent the self-reinforcing failure where an evicted texture samples a fallback, stops reporting its real requirement and never returns. Predictive bounds, persistent logical texture identity and explicit fallback feedback are possible mechanisms; validate one rather than relying on GPU samples alone. Account for upload mip bias when translating shader-reported mip numbers to source levels.

Begin with whole-image/coarse-tail replacement if it is the smallest portable correct implementation. Replacing an image must keep the old valid resource until the new one is uploaded and all descriptor consumers are updated safely. Maintain slot-lease contracts and reject late results belonging to reused identities. Budget old+new+staging overlap. Merely changing a view or sampler to use fewer mips does not demonstrate freed allocation bytes.

Preserve compressed paths and correct alpha coverage, normal maps, layered materials and shadow sampling. Do not turn cutout foliage opaque while classifying/loading a coarse texture. Reuse source-decoding/composition caches and include all transform/configuration dependencies in their identity.

Texture tiles/virtual texturing are a separate possible later project, justified by measured spatial texture waste. They are not prerequisites for geometry clustering. [E1]

### Stage F — Better conventional geometry and far representations

Establish authored LOD preservation from XOB/P3D through model IR, adapter, retained registration and GPU selection. Determine whether all LOD payloads remain allocated even though only one is selected. Culling a draw does not itself reclaim geometry memory. Verify shadows, reflection views, cutouts, animated parts and CPU fallbacks.

Prefer tested improvements in mesh sharing, conventional per-LOD residency and existing far representations before adding fine cluster paging. If generating simplified LODs, restrict to visual geometry, preserve material/UV boundaries and openings, and retain authored collision/fire/view/path/memory LODs untouched.

HLOD or impostors may help specific distant structure/vegetation groups, but they are not automatically appropriate for every asset. Keep doors/destruction and near/far handover coherent; a proxy must not resurrect a wreck or leak walls over a doorway. Measure polygon cost, material cost, draw submission and alpha overdraw separately.

Keep terrain-detail streaming as its own analysis. A heightfield collision sample, a rendered terrain patch and an object mesh have different lifetimes and error requirements. Do not promise to fix terrain-grid fidelity by clustering buildings.

### Stage G — Optional hierarchical geometry-page experiment

**27 September 2026 refinement following the owner's RTX Mega Geometry question.**
Keep the current shared streamer and bounded authored-LOD page prototype. For
the later continuous-LOD bake, evaluate pinned upstream meshoptimizer
`demo/clusterlod.h` first, using NVIDIA's Apache-2.0 `vk_lod_clusters` as a
technical reference. Do not invent a replacement simplifier or import the
RTXMG SDK into the core. This changes the preferred algorithm source, not the
correctness/performance acceptance gates. The owner has separately authorized
the independent opt-in prototype to proceed alongside unfinished residency work;
that authorization does not promote experimental paging to the normal renderer.

The upstream header supplies cluster/group generation and a spatial hierarchy.
It does not supply OpenPoseidon's asset eligibility, P3D material semantics,
submission lifetime, simulation ownership or WGPU page consumer. NVIDIA's sample
uses mesh-shader rasterization; RTXMG's related Cluster LOD path is ray tracing
only. Our first consumer remains conventional indexed WGPU draws, with bounded
compute/indirect refinement only after a resident reference works. No NVIDIA
hardware or mesh-shader requirement is introduced.

Implementation sequence for this refinement:

1. Finish source-to-page validation and an opt-in fully resident authored-LOD
   draw fixture. Preserve the original mesh and CPU helper LODs as the reference.
2. Evaluate an isolated offline/worker CLOD bake adapter for rigid opaque pilots.
   Pin library/fork versions and notices before importing code. Budget input,
   output, temporary allocations and worker concurrency separately; callback
   output limits alone do not establish a peak-memory or cancellation bound.
3. Preserve UV, normal and material seams and authored openings; reject
   unsupported selections/proxies/animation/destruction. Treat simplification
   error as an algorithm metric requiring visual validation, not proof of exact
   appearance preservation. Start without lossy vertex/UV quantization.
4. Validate the group DAG's paired selection predicates, monotonic group error
   and conservative bounds against a CPU reference. The current atomic switch
   between two complete authored LODs is not proof of a valid per-cluster DAG cut.
   Residency publication must preserve a complete fallback during partial group
   loads; equal main/depth/shadow/reflection geometry remains a separate gate.
5. Add bounded asynchronous demand, staging and retirement, then pressure control
   with hysteresis and measured equivalent quality. Count persistent coarse
   geometry, metadata and in-flight bytes in our total budget. NVIDIA's sample
   excludes persistent fallback bytes from its dynamic budget; do not inherit
   that accounting gap or its untuned thresholds as accepted defaults.

Verified primary references:
[meshoptimizer CLOD header](https://github.com/zeux/meshoptimizer/blob/master/demo/clusterlod.h),
[NVIDIA LOD generation](https://github.com/nvpro-samples/vk_lod_clusters/blob/main/docs/lod_generation.md),
[streaming lifetime and budget limitations](https://github.com/nvpro-samples/vk_lod_clusters/blob/main/docs/streaming.md),
[RTXMG rendering-path distinction](https://github.com/NVIDIA-RTX/RTXMG/blob/main/docs/ClusterLOD.md).
The dated review records pinned revisions and dependency/license details.
No dependency, runtime/default change or performance acceptance is established
by this plan amendment.

Proceed only if Stage F still demonstrates a material geometry-processing or geometry-residency bottleneck on representative content.

Nanite is a useful design reference: its public description includes hierarchical triangle clusters, view-dependent refinement, compressed fine-grained streaming and a specialized rendering path. That is substantially more than a distance-based object loader. Our experiment would be an independent, narrower design, not an Unreal integration or a claim of feature/performance parity. [E1]

Limit the first experiment to a few static opaque rocks or building shells. Exclude skinned characters, vehicles, moving doors, destructible topology, translucent surfaces, alpha-card foliage and terrain initially. This scope is an engineering choice, not a claim that modern Nanite cannot support those categories. Epic now documents dedicated experimental foliage technology; do not base a plan on old blanket statements about Nanite limitations. [E2]

Prototype in this order:

1. Use a disposable versioned derived cache, automatically generated from mounted original assets. Include producer/format versions, source identity and relevant material/geometry options. A corrupt/missing/unwritable cache falls back to the normal path. Do not require manual conversion or modify source PAKs/PBOs. [R14]
2. Build meshlets and bounds, then prove resident-only cluster culling worthwhile. Meshoptimizer is a possible building-block library, not a complete streaming renderer; verify any dependency version and licensing before use. [E4]
3. Build a simplification hierarchy with validated error bounds and compatible boundaries. Arbitrarily decimating each independent meshlet can produce cracks; independent detail replacement needs a coherent hierarchy/frontier.
4. Introduce compressed geometry pages distinct from clusters. Page granularity should amortize I/O/decompression and may hold multiple clusters. Reserve a valid coarse fallback before displaying the asset.
5. Traverse/select a consistent resident frontier. If finer children are unavailable, draw an eligible resident ancestor; do not draw holes or overlapping parent+child surfaces. Deduplicate requests, prioritize visible error and validate safe page eviction.
6. Extend the current compute/indirect rendering path where suitable. Do not require mesh shaders for the first WGPU prototype across supported adapters. The declared wgpu 29 line documents experimental mesh shaders, but support and shader-compilation paths are device/backend dependent. Capability-probe any optional fast path and preserve the conventional WGPU alternative. GL33 support or feature parity is not required. [R16, E3]
7. Keep simulation geometry/state independent. Implement correct bounds, materials, depth, shadow passes and motion history before expanding coverage. Use conservative current visibility for newly loaded/teleported views so stale Hi-Z cannot suppress newly visible geometry indefinitely.

Acceptance requires an advantage over the improved conventional baseline at comparable quality, including cold-cache cost and memory—not merely fewer reported triangles. Negative results are useful: keep the working conventional path and record why the added hierarchy was not worthwhile.

### Workstream T — Bounded WGPU terrain/water efficiency check

**Scope and order.** Perform T0 during Stage A. Implement T1 only for a demonstrated avoidable cost in the current WGPU path or its shared preparation. No GL33 instancing adaptation, shader-source refactor or expanded GL33 tests are included. This workstream is not a prerequisite for shared streaming or geometry paging. Use Section 0 to verify the task baseline; do not assume `codex/squad-ai` remains current. The upstream PR is an idea reference, not authorization for a cherry-pick, backend rewrite or default change.

**T0 — Trace WGPU ownership and attribute cost.** Follow `Landscape::DrawRect` into `DrawGround` and `DrawWater`, the active `GetTerrainRenderer` / `GetWaterRenderer` implementations, modern submission and actual GPU pass execution. Recheck the early return identified in Section 2.5. Record the normal WGPU path and supported WGPU feature-disabled/capability alternatives. Independently verify that headless execution does not depend on drawing for simulation/resource correctness. Do not conflate renderer-interface absence with an intentional setting that disables drawing.

Use existing profiler regions/counters first. Where necessary, add gated counters for legacy segment cache hits/misses, generation, GPU finalization, `_wTable` work, modern patch/instance submissions and actual draws. Attribute them by caller and pass so grass or terrain cache reuse is not blamed on water. Separate CPU preparation/submission from GPU rasterization/shading and reflection/depth costs. Avoid synchronous GPU waits for instrumentation. Establish whether a pass consumes shared data even when it does not submit visible geometry.

The expected source-level result at the addendum baseline is **no legacy water-loop fall-through when `IWaterRenderer` is non-null**. Test it and report it as already handled if confirmed. Do not invent redundant-work savings or delete the necessary dispatcher just to produce a code change.

**T1 — Correct only a demonstrated WGPU/shared-preparation issue.** If another reachable caller unnecessarily builds legacy water geometry while the modern renderer owns that responsibility, fix the smallest ownership/preparation boundary. Maintain modern submission and existing valid WGPU alternatives. Do not globally remove segment generation or repurpose simulation water queries. A valid result can be a focused regression test and no behavior change.

Keep water visibility, coast clipping, sea-level animation, supported inland/off-map water, seabed visibility, reflections, depth/underwater effects and fog intact where those features exist in the current WGPU path. Preserve terrain edits and the physical water-height/query contract. Rendering performance work must not change buoyancy, collision, AI or authoritative state. Demonstrate equivalent coverage before claiming gains; shrinking water reach or rendering fewer effects is a quality trade, not removal of redundant work.

Only consider persistent cell/patch lookup data where the WGPU path still performs measured repeated work that existing data does not already eliminate. Do not import upstream fixed 8×8 GL33 batching, texture-array limits or shader preprocessing into WGPU. If lookup data is genuinely added or changed, validate integer/layout bounds and invalidation after terrain deformation, material reassignment, remount/world change and relevant water-range changes. A cached potential-water mask must not become stale after a crater or supported runtime edit, and a camera-dependent render mask must never become simulation water truth. Reuse current WGPU shader, ABI and rendering tests rather than create GL33 infrastructure.

**Workstream exit gate.** Deliver a short verdict: already handled, measured WGPU/shared-preparation correction, targeted regression-test addition, or not worthwhile. Include pinned local/upstream evidence as relevant, equivalent-coverage measurements, tested WGPU capabilities, correctness results and rollback. If T0 finds no actionable issue, close this workstream and continue Stages B–G. GL33 performance is neither a deliverable nor a reason to keep this workstream open.

## 5. Validation matrix and metrics

Record commit, build mode, executable/DLL pair, mounted assets and precedence, configuration, OS, GPU/backend/driver, memory controls, screen resolution, view distance, camera trace and cache state. Use supported deployment/launch tooling and the shared game lock. Do not compare mixed binaries or concurrent runs.

Minimum corpus/scene coverage:

| Test | Purpose |
|---|---|
| Native Reforger town and forest | Mixed buildings, attachments, layered materials, foliage and roads |
| Modern OPRW world | Existing common-stream regressions, including packed versus loose inputs |
| Original OFP/CWA mission | Legacy lifecycle and visual-only compatibility |
| Walk, fast vehicle/aircraft, stop/reverse, teleport | Throughput, prefetch misses, stale work and hysteresis |
| Stationary camera with budget changes | Pressure response independent of recentering |
| Detached camera with distant AI/vehicles/projectiles | Logical/query world independence |
| Damage/door change, eviction/revisit, save/reload | Stable identity and mutable-state persistence |
| World switch while work is queued/converting/uploading | Generation, cancellation and resource lifetime |
| Texture eviction/reupload and slot reuse | No permanent white surfaces or stale bindings |
| Constrained budget and near-capacity stress | Real byte convergence, overshoot and no thrashing |
| Headless/dedicated-server control and supported MP fixture | Simulation/resource independence from rendering and authority boundaries |
| Supported WGPU adapters and optional-feature-disabled controls | Correct WGPU alternatives without requiring GL33 feature parity |
| WGPU modern-water interface present versus supported WGPU feature-disabled paths | Exactly the intended submission; no legacy water-loop fall-through when delegated |
| Coastline, underwater view, inland water where supported, and off-map ocean | Equivalent water/terrain coverage, sea level, depth, reflections and fog |
| Terrain/material edit, remount and boundaries of changed WGPU lookup data | Lookup/mask invalidation, safe indexing and no missing surfaces |

GL33 is not a performance, feature-parity or new-test-matrix target. Where shared changes touch code still built for it, apply only the minimal existing compatibility checks in Section 0.1. Headless/server and WGPU capability coverage remain required regardless of GL33 retirement.

Measure frame-time distribution (median/p95/p99/max and long-frame counts), attributed main-thread work, CPU/GPU time, request-to-usable latency, cold versus warm misses, upload/staging bytes, live/capacity/retired allocations, queue bytes and counts, number of redundant reloads, cancellation/drop reasons, desired/resident coverage and fallback time.

Record simulation progress alongside frame timing. Lower FPS can slow admission and influence what has loaded at a screenshot timestamp. Compare both the same camera/simulation trace and time to reach equivalent coverage. Never claim a speedup produced by not loading the test objects.

Repeat key cases; include instrumentation-off controls. Use performance thresholds selected from the measured baseline and target hardware rather than invented universal FPS guarantees. Automate correctness assertions where possible; visual acceptance still matters for popping, missing doors, foliage coverage and LOD seams.

## 6. Agent ownership, scope and delivery

Read `CLAUDE.md`, `.agents/README.md`, `.agents/AGENT_BOOTSTRAP_AND_DIAGNOSTICS.md` and applicable current handovers first. Follow Section 0 to discover and verify the active development baseline; reconcile stale trunk/branch references rather than silently switching to a historic/default branch. Repository search results may be indexed on another branch: re-fetch definitions at the pinned working commit.

Several inspected headers retain historical statements contradicted by later executable code. Update misleading comments only with evidence and keep documentation separate from behavior changes where practical.

Suggested isolated worktree/branch: `streaming/residency-audit-001`, created from the verified current baseline established in Section 0; use a unique name if occupied. Subsequent focused branches may be `streaming/residency-pressure-001`, `streaming/texture-detail-001` and `render/geometry-pages-prototype-001`, based on the applicable reviewed predecessor/integration commit. These are proposed feature-branch names, not instructions to replace the active development line, existing branches or public API names.

One integration owner should control `LandSave.cpp`, preparation state transitions and renderer ownership boundaries. A second agent can build fixtures and analyze passive telemetry; another can review simulation/save/locality correctness. Parallel agents must not independently rewrite the same residency policy or share an index/build directory.

Assign Workstream T to a WGPU renderer reviewer initially, not another owner of the streaming scheduler. Keep any demonstrated WGPU/shared-preparation correction separate from residency-policy changes. A proposed branch name such as `render/ground-water-audit-001` is an optional new name from the verified baseline, never a fixed integration target. Do not allocate a GL33 improvement agent, branch or milestone. Coordinate edits to `LandscapeRender.cpp`, shared interfaces, profiler code and WGPU files with existing renderer agents. Preserve attribution and applicable notices for any upstream code actually reused.

Deliver the active-path report and first demonstrated correction before broadening the work. Implement in small reversible commits with explicit test results and flags. Default-enable neither experimental pressure control nor geometry paging until its acceptance requirements pass. Keep renderer work out of unrelated AI/foliage changes; do not alter source content or discard newer installed work.

If runtime execution is unavailable, complete source analysis and runnable tests/procedures, state that limitation and leave performance claims unverified. Do not treat documentation, an enum, a slider or a compiled shader as proof that every production asset takes that path.

Final agent report must state: exact baseline and changes; enabled/default/experimental status; tested corpus/backend; coverage and correctness results; memory and latency changes at comparable quality; untested risks; fallback/rollback and actual installed build status if deployment occurred.

## 7. Source map

References R1–R16 and their repository links remain pinned to the original historical source-review commit. Supplementary A1 and U1–U2 use the separate commits explicitly recorded below for the terrain/water addendum; upstream GL33 evidence is retained only to explain the reference, not to schedule backend work. Neither set asserts a full review of the branch active when implementation begins. Line ranges are navigation aids. Re-resolve definitions and verify findings at the working commit discovered in Section 0 before implementation.

[R1] `engine/Poseidon/World/Terrain/LandLoadEnfusion.cpp`, native loader and material source chain; inspect the full loader, particularly lines 2000–2440 for stream setup and attachment handling.  
[R2] Same file, lines 2260–2440: model-table/placement registration, preparer setup, native budget.  
[R3] Same file, approximately lines 2650–end: road shape creation and load completion.  
[R4] `engine/Poseidon/World/Terrain/LandSave.cpp`, lines 1–240 legacy object creation; lines 2200–2430 modern stream setup.  
[R5] Same file, lines 3200–3480: `GpuClampedObjectBudget`, residency early exit and predictive center.  
[R6] Same file, lines 3720–3990: admission gates, bank tables and worker conversion handoff.  
[R7] Same file, lines 3480–3720: candidate invalidation, desired cells and live-object eviction.  
[R8] `engine/Poseidon/World/Terrain/ObjectStreamPrepare.hpp` and `.cpp`; particularly `.cpp` lines 290–640 for native archive and texture preparation.  
[R9] `engine/Poseidon/Graphics/Textures/PreparedTextures.hpp`: byte-bound store and generation/eviction contracts.  
[R10] `engine/WgpuRenderer/TextureBankWgpu.cpp`, lines 1–225: feedback reporting and texture budget; around lines 560–600: `UseMipmap`.  
[R11] `engine/WgpuRenderer/TextureWgpu.hpp`: lazy upload, pins, eviction and stable slot leases.  
[R12] `engine/WgpuRenderer/rust/src/gfx3d/cull.rs` and `cull.wgsl`: conventional GPU instance/model/LOD/section path; trace dispatch through renderer callers.  
[R13] `engine/WgpuRenderer/rust/src/gfx3d/mip_feedback.rs`: rotating nonblocking feedback; trace all consumers.  
[R14] `engine/Poseidon/World/Model/ModelDerivedCache.hpp`: derived-data key and producer contract.  
[R15] `engine/Poseidon/Dev/Diag/StreamingDiag.hpp`: shared diagnostics, active/disabled bound semantics and counters.  
[R16] `engine/WgpuRenderer/rust/Cargo.toml`: declared wgpu 29.0 dependency; verify resolved lockfile/device capabilities.


### Pinned repository links

- [engine/Poseidon/World/Terrain/LandLoadEnfusion.cpp](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/e486e7191579ea9d3e5a0c0c09d34f417c944f80/engine/Poseidon/World/Terrain/LandLoadEnfusion.cpp)
- [engine/Poseidon/World/Terrain/LandSave.cpp](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/e486e7191579ea9d3e5a0c0c09d34f417c944f80/engine/Poseidon/World/Terrain/LandSave.cpp)
- [engine/Poseidon/World/Terrain/ObjectStreamPrepare.hpp](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/e486e7191579ea9d3e5a0c0c09d34f417c944f80/engine/Poseidon/World/Terrain/ObjectStreamPrepare.hpp)
- [engine/Poseidon/World/Terrain/ObjectStreamPrepare.cpp](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/e486e7191579ea9d3e5a0c0c09d34f417c944f80/engine/Poseidon/World/Terrain/ObjectStreamPrepare.cpp)
- [engine/Poseidon/Graphics/Textures/PreparedTextures.hpp](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/e486e7191579ea9d3e5a0c0c09d34f417c944f80/engine/Poseidon/Graphics/Textures/PreparedTextures.hpp)
- [engine/WgpuRenderer/TextureBankWgpu.cpp](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/e486e7191579ea9d3e5a0c0c09d34f417c944f80/engine/WgpuRenderer/TextureBankWgpu.cpp)
- [engine/WgpuRenderer/TextureWgpu.hpp](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/e486e7191579ea9d3e5a0c0c09d34f417c944f80/engine/WgpuRenderer/TextureWgpu.hpp)
- [engine/WgpuRenderer/rust/src/gfx3d/cull.rs](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/e486e7191579ea9d3e5a0c0c09d34f417c944f80/engine/WgpuRenderer/rust/src/gfx3d/cull.rs)
- [engine/WgpuRenderer/rust/src/gfx3d/cull.wgsl](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/e486e7191579ea9d3e5a0c0c09d34f417c944f80/engine/WgpuRenderer/rust/src/gfx3d/cull.wgsl)
- [engine/WgpuRenderer/rust/src/gfx3d/mip_feedback.rs](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/e486e7191579ea9d3e5a0c0c09d34f417c944f80/engine/WgpuRenderer/rust/src/gfx3d/mip_feedback.rs)
- [engine/Poseidon/World/Model/ModelDerivedCache.hpp](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/e486e7191579ea9d3e5a0c0c09d34f417c944f80/engine/Poseidon/World/Model/ModelDerivedCache.hpp)
- [engine/Poseidon/Dev/Diag/StreamingDiag.hpp](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/e486e7191579ea9d3e5a0c0c09d34f417c944f80/engine/Poseidon/Dev/Diag/StreamingDiag.hpp)
- [engine/WgpuRenderer/rust/Cargo.toml](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/e486e7191579ea9d3e5a0c0c09d34f417c944f80/engine/WgpuRenderer/rust/Cargo.toml)

### External primary references, checked 19 September 2026

[E1] [Epic: Nanite Virtualized Geometry Overview](https://dev.epicgames.com/documentation/unreal-engine/nanite-virtualized-geometry-in-unreal-engine) and [Nanite Technical Details](https://dev.epicgames.com/documentation/unreal-engine/nanite-technical-details). Public design references, not authorization to copy engine implementation.

[E2] [Epic: Nanite Foliage](https://dev.epicgames.com/documentation/unreal-engine/nanite-foliage). The retrieved documentation marks the foliage system Experimental.

[E3] [wgpu 29.0.0 Features](https://docs.rs/wgpu/29.0.0/wgpu/struct.Features.html), especially `EXPERIMENTAL_MESH_SHADER` and `MULTI_DRAW_INDIRECT_COUNT`. Support is conditional on the adapter and backend.

[E4] [meshoptimizer project documentation](https://github.com/zeux/meshoptimizer), particularly meshlet building/bounds and simplification. Candidate building blocks, not a complete Nanite implementation.

### Supplementary terrain/water evidence — 19 September 2026

[A1] [OpenPoseidon `LandscapeRender.cpp`, pinned addendum review](https://github.com/koosoli/PoseidonWGPU-private-safety-mirror/blob/f08ff0d7674e066944f0cf6195ae39e2e94f48aa/engine/Poseidon/World/Terrain/LandscapeRender.cpp#L1217-L1275). The entry to `Landscape::DrawWater` delegates to a non-null `IWaterRenderer` and returns; legacy segment-cache/`_wTable` work follows below. This verifies the conditional routing in source, not every runtime configuration. Remote branch `codex/squad-ai` resolved to this commit for this targeted check; still discover the actual active task worktree under Section 0.

[U1] [CWR-CE PR #314 — Porting instanced terrain (water) rendering](https://github.com/ofpisnotdead-com/CWR-CE/pull/314). Metadata checked through the GitHub connector: open/unmerged, head `6469b7f289e6620aae48ca367d15280976849117`, base `ce7c153746d659359260934c5470e46650e81f83`. Status is a dated observation; refresh only if selecting a specific idea/code fragment for WGPU/shared-service reuse, not to port GL33. No upstream benchmark was independently reproduced for this addendum.

[U2] [Upstream `EngineGL33_Terrain.cpp` at the reviewed PR head](https://github.com/ofpisnotdead-com/CWR-CE/blob/6469b7f289e6620aae48ca367d15280976849117/engine/PoseidonGL33/EngineGL33_Terrain.cpp). Historical reference for reusable segment geometry, texture-array grouping, cell metadata and water-segment filtering. No GL33 port, tuning, index/batch-limit hardening or shader work is requested. Reuse an idea only when it addresses a measured WGPU/shared-service issue; do not copy the backend or its constraints.
