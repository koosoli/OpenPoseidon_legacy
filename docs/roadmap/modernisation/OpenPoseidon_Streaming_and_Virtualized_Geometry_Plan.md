> Execution note, 20 September 2026: work proceeds in `codex/streaming-residency`,
> based on verified `03e713714b9b699e3f6f4eae7cdce7f488a13b64` in the existing
> `combat-audit` worktree. The historical branch examples below are not switch targets.
> Waves are parked at the owner's request; `safety/codex/backup-waves-20260920`
> preserves that baseline. Historical 20 September default: old ocean.
> Owner-directed 27 September promotion supersedes this: Tidewater is default;
> explicit backend0 preserves legacy. See TW-WATER-14-PENDING-20260927.md.
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

**Verified checkpoint, 27 September 2026, installed build 86971cce at 11:06.**
The repaired five-target build and focused suite passed 30,600 assertions in
60 cases without skips, including bounded source export, page/whole-representation
decoding, CPU CLOD bake and exact cut decoding. The initial narrowing fixture
fact-count build failure was repaired by 8b42afdb. At this checkpoint meshoptimizer linked only into
Tests. The actual game fixture uses programmatically authored box Shapes rather
than meshoptimizer output or arbitrary retail assets. It proved fixed-instance
whole 12/48-triangle switching, 74,992 logical bytes/three mesh records, unchanged
CPU helper hashes and Present-to-Absent record lifetime. The disabled control
used zero fixture payload; private standalone/shared lifetime regressions passed.
Root inspected matching black-box silhouettes, which do not establish material,
pixel or all-pass parity. See
[the CLOD pilot checkpoint](../decisions/GEOMETRY-CLOD-PILOT-20260927.md) for hashes.

The subsequent `ownership-returns-20260927-20260927-111103` route passed the original
convergence thresholds with quiet returns and zero retired geometry. Geometry
fell by 3,993,516 bytes between returns 2/3. Post-quiet record cuts closed with zero
logical residual, while truncated asset/LOD rows increased their sampled byte
union: they are not whole-pool or exclusive ownership evidence. This repeat does
not erase the earlier ResourceGrowth failure of 15,338,464 bytes, prove a general
memory bound, or establish causal performance/no-regression acceptance. The extra
bounded inter-route diagnostic intervals remain part of the test conditions.
Stage G and the whole plan remain OPEN; experimental paging/CLOD stays off by
default, with complete ownership, physical reclaim and pressure gates unresolved.

**Later resident CLOD checkpoint, installed bdb02bb8+dirty at 11:18.**
The dirty label records documentation only; functional source is exact bdb.
Five-target build passed 257 scheduled steps; focused tests passed 6,058 assertions
in 11 cases. Explicit `BeginClod` now links Renderer and submits actual paired
coarse/fine 64/512-triangle cuts from four groups/12 clusters, with an independent
authored two-triangle fallback. Eight mesh records were Present then Absent,
three models switched on one instance, and CPU helper hashes remained unchanged.
Known stored bytes were 351,017, excluding full scratch/peak; one explicit local
bake took 1.1831ms without a hard latency guarantee. Disabled CLOD and common
12/48 authored publication regressions passed with clean exits (latter 83,896
known stored bytes). Root's curved black silhouettes do not prove material/pixel
or all-pass equivalence. Normal frames perform no bake; the startup fixture gate
remains off. Disk backing, async paging, eviction/refill, complete ownership,
physical reclaim and performance/default acceptance remain unresolved. Actual
build/runtime hashes are linked in the CLOD pilot decision above.
**Authored-page worker checkpoint, clean installed 28248174 at 11:50.**
Five-target build passed 252 scheduled steps; C++ checks passed 30,683 assertions
in 64 cases without skips and Rust pass-fact checks passed five tests. Actual
held epoch1 decode kept its coarse fallback and 172,040 scheduling bytes through
Abort. Replacement epoch2 queued beside that cancelled active job (two jobs,
344,080 reserved bytes); release published only epoch2, three Present fine mesh
records, with final jobs/reservation zero and retirement to three Absent records.
Known stored fixture bytes were 54,088 coarse /63,224 fine, not full scratch/RSS.
CLOD common-path regression and disabled worker controls passed with clean exits.
Copied version1/576-byte actual command facts show four cascades/current epoch;
stale interior/GI and unsupported reflection remain Pending. Observations now
require real renderer return0; this does not prove survivors, pixels, queue
completion or all-pass equivalence. Immutable RAM-backed authored pages are the
only worker source; no disk/retail/autopaging/default promotion is implemented.
Default optional fields/branches are not zero cost. Fine eviction/refill remains
future work; all ownership/reclaim/pressure/performance/default gates stay OPEN.
Hashes and actual checkpoint scope are recorded in the CLOD pilot decision.

**One-cycle fine eviction/refill and reflection checkpoint, clean 3d720430 at 12:18.**
The five-target build (249 scheduled steps), C++ checks (16,004 assertions in
31 cases) and seven Rust tests passed. Actual fine eviction preserved three
historical handles: one Present coarse and two Absent old fine records.
Cancellation retained 172,040 bytes of held refill scheduling debt. Fresh
generation2 published two new handles/model: five historical records, three
Present and two Absent. Finally all five were Absent with jobs/reservation zero.
Stored fixture contents of 64,056 bytes exclude physical GPU payload, RSS, peak
scratch and reclaimed bytes; a second cycle is refused. The CLOD regression
records actual reflection commands with version2/592-byte facts and current
frame/instance epoch; cached interior/GI remain honestly Pending. No pixel,
survivor, queue or all-pass certification follows. The disabled control passed
with zero payload/history. The candidate-only Disabled/Counted latency diagnostic
passed735frames/drop0: mean48.995ms, median24.372ms, p95166.552ms, p99467.457ms,
max938.199ms, returned19,923/required0. Large draw-init/terrain/swap hitches remain.
No matched baseline, causal no-regression or performance acceptance is established;
the unsupported raw-swap ABA helper remains unexecuted.
Defaults stay off. Disk/retail/autopaging, ownership, reclaim, pressure and
performance acceptance remain OPEN, as does the whole plan. Actual hashes are
recorded in the linked pilot decision.

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

## Verified selected-CLOD RAM foundation and default section refresh, 12:43

Clean installed `51f4e5e0` contains `1834a338` selected-CLOD RAM adapter/worker
foundation and `e631ffee` section-refresh validity, with the repaired Rust key
annotations. The initial three key-type inference errors are preserved in the
failed test log. The initial 21-scheduled build and repaired five-target 18-scheduled
build passed. C++ tests passed 19,160 assertions in 18 cases with no skips;
Rust section-refresh tests passed seven cases, including the explicit actual-GPU
upload/readback PASS marker and same-capacity full-generation recreation. The
expected caught-panic test does not indicate an uncaught test failure.

The selected-CLOD adapter recomputes paired selections and verifies exact original
`SVertex` attributes, selected topology and single material before publishing a
transactional RAM package. Its reference is the complete simplified SELECTED CUT,
not the unsimplified fine representation. Derived identity includes source,
library/adapter policy, thresholds, cut IDs and exact geometry. Caps remain eight
pages, 64 clusters, 1,024 vertex records/cut, 4,096 indices/cut and 128KiB known C++
capacity, excluding external library scratch, allocator overhead and RSS. Actual
worker units decode these selected-cut bytes; held publication cancellation keeps
old source/reservation debt, rejects old epoch/request results, and publishes only
the changed-source replacement. This checkpoint proves CPU adapter/worker tests;
combined CLOD runtime paging is still unbuilt and excluded from installed51.

Default section-address refresh uses a 16-byte scalar validity latch and O(1)
pool-generation comparison. Structural mesh/table mutations invalidate it; clean
frames skip section resolution, vector allocation and equality comparison while
normal cull/upload/animation work continues. Tests share the production closure
and prove equal addresses against always-resolved output, including missing full
generational handles. Exact startup `WGR_CULL_SECTION_REFRESH=0` retains the
legacy per-frame refresh body; only that exact string forces rollback. Provable
work-count reduction is not a measured FPS improvement.

The real authored page lifecycle `section-refresh-page-lifecycle-20260927-20260927-124430`
passed with a clean exit: held job1/reservation172,040 survives Abort, replacement
queues two jobs/reservation344,080, only the current fine result publishes, the
one-cycle eviction/refill preserves five-handle history, and final five Absent
records/job0/reservation0 are verified. This remains record lifetime, not queue
completion, device-free, physical-reclaim or all-pass proof.

Two same-binary/script/HEAD Disabled/Counted DayZ arms passed with clean exits and
zero dropped frames. The legacy arm sets refresh0; optimized sets refresh1.
Provenance confirms identical other configured settings (private user paths differ).
Raw timings use ordinary median and nearest-rank ceil(n*p)-1 tails:

| Arm | Frames | Mean ms | Median ms | p95 ms | p99 ms | Max ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Legacy124557 | 720 | 50.050686 | 26.809 | 170.123 | 439.533 | 1045.527 |
| Optimized124850 | 725 | 49.801603 | 26.063 | 165.745 | 451.559 | 1217.254 |

Both returned19,923 residents/required0/pendingfalse, pool capacity1,275,068,416.
Legacy geometry713,281,452 versus optimized710,383,980 bytes; textures
2,748,656,436/2,488 versus2,748,874,868/2,490 entries; backend allocation
5,301,160,464 versus5,301,382,352. The ordered arms have unequal allocated work
and cache/async history. Optimized p99/max are worse, despite lower mean/median/p95;
this mixed screen establishes no causal benefit, no-regression or performance
acceptance. No thresholds were relaxed. Stage G/full plan remain OPEN;
performanceAccepted/defaultReady remain false, experimental paging stays OFF.


Artifact hashes are recorded in `../decisions/GEOMETRY-CLOD-PILOT-20260927.md`.

## Combined CLOD RAM worker paging checkpoint, 13:11

Clean installed `c072453c` integrates `3d4874c9` combined selected-CLOD RAM
paging and `e9d51eb8` dirty GPU table domains. Initial 246-scheduled build failed
with two errors from a shadowed `ClodCut selectedCoarse`; the preserved failure
log precedes the four-line naming repair and successful five-target20-step build.
C++ policy tests passed15,269 assertions/11 cases. Two actual-GPU table tests
passed with explicit readback/bootstrap PASS markers/no adapter skip; the shared
upload-closure pure test also passed separately. These are the actual tested scopes.

The optional combined fixture now retains four distinct model birth IDs on one
instance: independent authored fallback0, selected coarse1, worker fine2 and
refill3. Selected coarse64/fine512 triangles use the paired CLOD cuts. Derived RAM
reference is the selected simplified geometry, not original fine coverage. Raw
source SHA `f0dc3b432424e1323480f6bd6c9c6870935d631df7074d39d2895512a15f45c4`
remains separate from derived selected-cut SHA
`02ec881ef4b63ff9803c59f299b542554a26eb46cba3d2b8c5b6f92f1a97948e`.
Retained RAM package known capacity88,974 bytes and copied worker input76,528
bytes are both charged; source caps128KiB each, fixture1MiB, pages8/history64 and
worker2-job bounds remain. Known startup payload371,575 grows to406,543 after fine
publication; these are stored capacity estimates, not full transient/RSS/GPU peaks.

Actual `clod-paged-eviction-cull-domains-20260927-20260927-131155` passed with
clean exit. Held epoch1/job1/reservation223,984 survives Abort; replacement has
two jobs/reservation447,968. Only epoch2 fine publishes. Birth fine produces
history8/eightPresent. Eviction leaves twoPresent/sixAbsent; a cancelled refill
keeps job1/debt223,984, replacement request retains two jobs447,968 until old
work exits. Generation2 appends six fresh handles, history14/eightPresent/sixAbsent;
old history remains unchanged. Authored fallback returns on the same producer
instance0/renderer instance16,777,217 with its original model. Final Abort verifies
fourteenAbsent/jobs0/reservation0. No weaker record Ready contract was introduced.

Copied pass facts remainv2/592, capability31/required29. Actual coarse frame83/
epoch7 and fine frame96/epoch8 match their reflection command stamps; cached
interior/GI bits honestly remain Pending12, returned fallback Pending4. Successful
main return and exact current model/instance/handle records prove command and
record lifetime only, not nonzero survivors, pixels, all-pass, queue completion,
device-free or physical reclaim. No new root visual judgement is claimed here.

Three additional clean-exit controls passed: authored worker/eviction131320 final
fiveAbsent/debt0; resident CLOD131335 fallback/coarse/fine switching and final
eightAbsent with known351,017 bytes; disabled131349 zero fixture payload/history.
The fixture gate remains OFF by default; normal frames perform no bake/page jobs.

Default dirty-table mask replaces the old bool with one byte. Registration marks
ALL, retirement LOD, section replacement SECTION, crown append CROWN, feedback
MODEL. First dirty crown/section prepare initializes all five backing buffers;
clean NONE avoids buffer probes and upload callbacks. Every buffer growth still
rebuilds shared main/cascade/reflection/sky/color views. Pure/actual-GPU parity
supports correctness, but default traversal performance for this new mask is
UNMEASURED. Earlier51 mixed screen is historical, not a test of this mask.
Future current-call timing and retirement-view patches are excluded from c072.
Whole plan and Stage G remain OPEN; performanceAccepted/defaultReady false.
General asset/DAG frontier, disk paging, automatic pressure policy, full GPU
ownership, reclaim and all-pass acceptance remain outstanding.


Exact hashes are recorded in `../decisions/GEOMETRY-CLOD-PILOT-20260927.md`.

## Current CPU attribution and retained-content correctness, 13:26

Clean installed `abd75075` includes unchanged section-resolver extraction and
actual stale-model main/shadow/color GPU testing (`dddf5df6`), caller-owned current
render CPU timings/ABI16 (`c1af1457`), exact startup diagnostic opt-in and strict
runner OFF checks (`3d74e072`, `ab92017c`), model-retirement retained-content epoch
invalidation (`c1608923`) and unchanged Crown upload suppression (`abd75075`).
The earlier26-step five-target build passed but was not installed; final18-step
five-target build passed. Eight Rust tests passed with no failures/ignored/adapter
skips, including three explicit actual-GPU PASS markers. Expected caught upload
panic remains a passed refusal test, not an uncaught build/runtime error.

Only a retirement that changes nonzero LOD section counts advances the existing
retained-content epoch. Repeated/unknown/already-empty retirement is inert; slot
identity, generation and instance upload ranges remain unchanged. This closes
that model-retirement cache-key seam, not arbitrary mesh/pose/dynamic changes or
all cached-image pixel correctness. Actual GPU tests keep stale A's instance,
retire A, retain B, then reuse A's mesh slot at a new full generation without pool
relocation and verify exact B/C addresses across main/shadow/color. Model append
marks four changed upload domains, preserves pending Crown, and initialized Crown
GPU sentinel/capacity stays untouched until a real Crown append. Missing-buffer
model/crown/section bootstrap and shared-view growth rebinding remain verified.

Current CPU facts use an additive matched ABI16 function and caller-owned72-byte/
v1 snapshot. Exact cached startup `WGR_RENDER_CALL_CPU_TIMINGS=1` plus the existing
slow-block gate opts in; slow-block itself defaults100ms and therefore is not an
opt-in. OFF calls the original render entry and reads no uninitialized snapshot.
Completed acquired frames copy the current accumulators after present/harvest;
skipped acquire and failed/panicked calls clear unavailable facts. Old latest
getter semantics remain historical. Token identifies the call, not GPU completion.
No retained renderer snapshot/heap/new clocks; caller stack/gate branches are real
costs. Foreign layout/token refuses before sentinel dereference; ABI15 is refused.

Actual combined paging `combined-pages-retained-epoch-20260927-20260927-132718`
passed with clean exit, final14Absent/job0/reservation0 and retained-content epoch21.
Independent fallback/paired coarse/fine/refill, cancellation debt and historical
handles remain intact. Record/command facts are still not physical-reclaim,
queue-completion, nonzero-survivor, pixel or all-pass proof.

Actual same-binary DayZ profile132735 and normal-diagnostic-OFF133030 passed with
clean exits/zero dropped frames. Profile collected seven paired current-call rows
from seven slow rows; OFF had four slow rows/zero paired rows and disabled timing
facts. Raw route metrics use ordinary median and nearest-rank ceil(n*p)-1 tails
(the tail values coincide with the root floor-index summary for these two counts):

| Arm | Frames | Mean ms | Median ms | p95 ms | p99 ms | Max ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Profile132735 | 727 | 49.024567 | 24.326 | 181.443 | 436.068 | 929.794 |
| DiagnosticOFF133030 | 793 | 44.17028 | 25.481 | 129.548 | 398.908 | 1107.168 |

Both returned19,923 residents/required0/capacity1,275,068,416. Profile geometry
722,708,572 versus OFF710,804,036 bytes; textures2,774,738,164/2,511 versus
2,749,704,948/2,491 entries; backend5,327,299,856 versus5,301,168,000.
Profiling overhead, ordered cache/async history and unequal allocated work forbid
causal performance/no-regression acceptance. The OFF maximum1,107.168ms remains
an actual hitch; this snapshot is not normal-default acceptance or vanilla parity.

The bounded parser separates startup from log completions inside the armed route
window (13:28:13.663–13:28:49.471). Four paired slow rows finish inside capture;
one records acquire697.201ms/setup-inclusive705.210ms, not a decompression claim.
Twenty captured warm rows include max texture-read695.743ms/total799.018ms.
Two captured terrain uploads include ensure737.488ms versus descriptor5.495ms,
load-owner3.007ms and total748.182ms. These same-field observations are not summed
across overlapping hierarchies, threads or boundary-spanning calls. CPU acquire includes the complete acquire() call: runtime diagnostics/pool/storage
event draining precede get_current_texture. The697.201ms observation cannot isolate
surface wait from that preamble. CPU submit/upload includes owner waits, not GPU
elapsed time. Mip-chain log cap128
saturated at startup13:27:56.857; there are zero captured mip-chain rows, so it
cannot classify the route texture-read bottleneck. Terrain sampled source is a
bounded source-family observation, not all texture coverage or overall throughput.

At the historical abd checkpoint, disk codec/capture-reset patches were unbuilt/excluded. Whole plan and
Stage G remain OPEN; experimental paging and CPU profiling default OFF,
performanceAccepted/defaultReady false. No global ownership/reclaim/pressure or
all-pass certification follows. Installed EXE SHA256
75B1B1978EFB6C423725FBB7B687757E8AFCDD45B885F0851527F225C99B0EB8;
DLL313FB39C72FF892D3EAF149AD848C1CAD0622CBB2C5C9966223339CD3D74FEB9.


Exact artifact hashes are recorded in `../decisions/GEOMETRY-CLOD-PILOT-20260927.md`.

## 451db9fe typed vertex codec and captured mip checkpoint (27 September, 13:55)

Clean installed451db9fe contains memory-only bounded CLOD disk codec287107a8,
first-capture mip diagnostic reset9f4b0837, typed resident assignmentef5297f6,
and field-wise LE repair451db9fe. Initial build failed its nontrivial-SVertex
assertion and remains recorded. Vector3P has a custom copy operation; the repair
serializes all17 field words in pinned68-byte layout and reconstructs valid typed
vectors/vertices. Resident decoding copies the original typed vertex after the
existing exact-byte witness. No raw byte writes into nontrivial vertex objects;
wire/hash policy, validation/caps and exact attributes remain preserved.
Repaired all-five-target build passed24 scheduled steps. Focused codec, typed
resident, representation, RAM adapter and worker tests passed18,290 assertions
in21 cases without skips, including LE signed-zero/subnormal field transport.

Actual typed-resident135554 passed with cleanExit=true. Selected coarse/fine64/512
triangles, RAM source88,974/copied worker input76,528 known bytes; final generation2
history14 entirely Absent, jobs/reservation0, helper hashes identical and known
payload406,543 bytes. Pass facts v2/592 capability31/required29 finalepoch21,
current cascade/reflection and interior pending4 remain command-recording evidence,
not all-pass pixels, submission completion or physical device reclamation.

DayZ captured-mip135612 passed and logged Shutdown complete13:57:33.547.
One accepted reset13:56:51.834 starts the extra bounded window; startup and capture
each emit128 slow-chain rows and their own truncation marker. This exercises the
diagnostic without complete route/source coverage. Eleven slow rows have eleven
paired current-call CPU rows. Ordinary median/nearest-rank ceil(n*p)-1 tails:
412 frames/drop0, mean87.908430ms, median54.4545ms, p95239.610ms,
p99651.986ms, maximum1,484.495ms. These poor observed timings remain explicit;
profiling/order/unequal allocated work prevents performance/no-regression acceptance.
Acquire1,175.276ms includes pre-surface diagnostics/pool/storage work, not isolated
surface wait. Mip cost conclusions await bounded phase/source analysis; no
route-wide decompression attribution is asserted. Returned19,923 residents,
required0/pendingfalse, geometry759,085,432/capacity1,275,068,416/retired0 bytes;
textures2,889,614,964 bytes/2,569 entries/backend5,444,114,784 bytes.
No new visual acceptance is asserted.

The disk filesystem worker is reviewed but NOT applied/installed in451. Memory
codec external original/derived keys authenticate captured content, not current
mounted-source freshness. No disk jobs, generalized asset paging, pressure policy
or normal-frame CLOD bake is enabled. Whole plan/Stage G remain OPEN, experimental
defaults OFF, performanceAccepted/defaultReady false. Earlier ResourceGrowth
failure and bounded repeat convergence pass remain separate historical evidence.
Installed EXE374E2CF5DBD23685262ACFB09C7A490CA15DFEB7DEDA9CE1E2A6203CEE988C88;
DLLECD07DE0C87225B248EE8CDC85F2056F6F72706DAFD99CBEC8C67793F94BE3CB.

Exact artifact hashes are recorded in `../decisions/GEOMETRY-CLOD-PILOT-20260927.md`.

## 7c426809 bounded disk worker and warm filename-family gate (27 September, 14:14)

Installed clean7c426809 contains disk worker19066746, optional existing material
resolver source-purpose scope66e2c756, upload witness8f886598 and local-name
compile repair7c426809. Disk/material build passed30 scheduled all-five-target
steps; filesystem worker/codec tests passed5,754 assertions in14 cases.
The initial witness build failed its out-of-scope name reference and is preserved;
repaired all-five build passed. Fresh source-purpose/warm queue tests passed
1,064 assertions in9 cases without skips. Material scope captures only actual
existing owner Init reads under optional flags; cached Unknown remains Unknown.

Filesystem worker validation here is CPU unit evidence only, with no Engine/game
file submission. Bounded worker owns a private const path/key snapshot, rechecks
copied native capacity/reservation and shares2-live-job/1MiB logical budget with
RAM jobs. Cancellation debt lasts through actual worker exit/result release;
private test cleanup verifies exact canonical child before removal. File-size
check precedes allocation and full memory codec precedes fine publication.
External keys authenticate captured bytes, not current source freshness; pathname
status/open TOCTOU, synchronous I/O and allocator/RSS/scratch limits remain.

Actual dayz-bound-stage-family-upload141421 FAILED its strict positive gate:
runner exit1/result.passed=false, while owned game shut down normally14:15:36.078.
One job submitted/completed, one member prepared/Put; Take0/Upload0 and empty
successful filename-family witness observations. Required stage-family positive
is false, not inferred from a queued job or prepared bytes. Failure result, proof,
raw frames, provenance/log and screenshot are retained. Final queued/active0,
job metadata/scratch0, optional queue storage1,056 bytes; PreparedStore still
one entry1,573,776 bytes (not whole-resource convergence). Peak metadata20,824
and scratch67,174,400 known bytes. No uploaded material role, readiness or gain
was established; fallback/capture/cap explanation remains under investigation.

Raw route695 frames/drop0: mean49.799590ms, ordinary median25.342ms,
nearest-rank ceil(n*p)-1 p95158.628ms/p99542.318ms/max1,069.360ms.
Three slow rows have three paired current-call CPU snapshots; acquire944.684ms
includes acquire preamble, not isolated surface wait. Profiling overhead, ordered
cache and unequal work forbid performance/no-regression acceptance. Returned
19,923/required0/pendingfalse; geometry731,868,124/capacity1,275,068,416/retired0;
textures2,883,832,468 bytes/2,633 entries/backend5,436,685,200 bytes.
No new visual acceptance is asserted. Upload witness only claims successful bound
source-validated block upload of filename family, not shader/material slot role.
Experimental jobs/source/witness remain OFF by default. Whole plan/Stage G OPEN,
performanceAccepted/defaultReady false. Reviewed offline producer is future
unbuilt/uninstalled work, excluded from this checkpoint.
Installed EXE046633FF02A68297B16A3A5235D7C1788A270DF0BEC6EECB7659DC6A7E9F052E;
DLL16323FFBE55029A28E96A7EA06BB7E161D29464964AD9F9F99A13D83F3C76C5A.

Exact artifact hashes are recorded in `../decisions/GEOMETRY-CLOD-PILOT-20260927.md`.

## bdc7519b offline producer, explicit disk frontier and cached stages (27 September, 14:24)

Offline producer13929948 passed all-five-target build45 scheduled steps and
4,154 assertions in2 controlled Shape/worker cases without skips. Actual Tools
CLI emitted a71,652-byte selected package and separate captured producer manifest.
It retains original raw SHA f0dc3b432424e1323480f6bd6c9c6870935d631df7074d39d2895512a15f45c4
and derived cut SHA02ec881ef4b63ff9803c59f299b542554a26eb46cba3d2b8c5b6f92f1a97948e,
matching the earlier controlled renderer input. Coarse64/fine512 triangles,
original289vertices/512triangles/fallback2, groups4/bakedclusters12/selected7,
source88,974 known bytes. This is controlled authored data, not retail source
eligibility, arbitrary DAG certification or GPU proof. The manifest is separate
producer provenance; stdout/file newline differences require semantic equality
and distinct artifact hashes, not byte equality or trusting cache-file keys.

Clean installedbdc7519b contains no-load cached-stage capture2af993a8 and explicit
disk frontierbdc7519b. All-five build passed20 scheduled steps; combined worker,
codec, producer and frontier tests passed11,331 assertions in18 cases/no skips.
Cached fallback inspects only existing material cache/texture-bank first normal
and specular slots, preserving first-slot refusal. No Resolve/Load/new source
reads, Unknown retrofit or cap increase. Cache/slot/bank/inspect visits share256
budget and64 accepted-read ceiling. Immutable disk metadata freezes Coarse/Fine;
RAM and old callers remain Fine, invalid enum refuses before job allocation,
failed Coarse retains only its requested-role witness with decodedUnavailable.
Filesystem worker remains CPU-unit validated, not invoked by an Engine game yet.

Actual cached-stage142502 FAILED its required successful-upload gate: runner1,
result.passed=false, normal owned Shutdown complete14:26:17.033. One submitted/
completed job, one prepared member/Put, Take0/Upload0 and zero successful filename-
family witnesses. Failure result/raw frames/proofs/provenance/log/image remain.
Final job metadata/scratch0, queued/active0, optional queue1,056 bytes, but store
one entry1,573,776 bytes persists. Peaks20,824metadata/67,174,400scratch bytes.
Source/capture/fallback limits remain under investigation; no material-slot,
readiness or gain is inferred from a prepared job.

Raw742frames/drop0, usual median/nearest-rank ceil(n*p)-1 tails: mean48.250581ms,
median24.281ms, p95150.836ms, p99542.500ms, maximum1,019.661ms; six slow rows/six
paired CPU rows. Returned19,923/required0/pendingfalse, geometry716,495,592 bytes/
capacity1,275,068,416/retired0; textures2,770,718,932 bytes/2,504 entries and
backend5,322,212,160 bytes. Ordered profiled unequal work prevents causal
performance/no-regression acceptance. No new visual acceptance. Experimental
warm jobs/source capture remain OFF; plan/Stage G OPEN/defaultReady and
performanceAccepted false. Future Engine disk-fixture patch is unbuilt/excluded.
Installed EXEFE201EDEC69E9341DA3EE20E6256BFC578025C8D3913559A194549791947C0C4;
DLL5BC75016807376DEBEA75A0F5F492C6D4B347E16B437D004D5499AF388B81540.

Exact artifact hashes are recorded in `../decisions/GEOMETRY-CLOD-PILOT-20260927.md`.

Actual producer output directory `build/controlled-clod-producer-20260927-1422`
was independently checked: parsed captured stdout and manifest.json are equal;
selected.gcd size/SHA match its producer manifest. Existing-directory CLI refusal
returned2 and preserved the selected-file hash. Original producer Tools EXE SHA
FD1A3009ACD6ED7EDB6C2F91A86913F5B48BF8CE585EE540A7F065C04B9CACB0
belongs to139's actual operation; later bdc Tools rebuild is not that binary claim.
Bounded cached-stage prefix observes293 slot visits/147 material-cache hits and
zero accepted stages; prefix counts are not whole-route coverage. No unchanged
experimental reruns or capacity promotion are justified by this failed gate.


### 095cee22 installed acquire attribution checkpoint (27 September,14:38)

095cee22 historical installed checkpoint (27 September,14:38): all-five-target build18stepsPASS; Rust acquire contract5testsPASS/noSKIP. Actual143822 screening PASS667frames/drop0, normal shutdown14:39:39.746 and returned19923/required0. Optional acquire split64rows+cap pairs token415 with successful state1/count12/status0: surface API544.730ms, diagnostic preamble0.001ms, split total544.731ms versus inclusive acquire544.789ms/render576.5ms. Token501 instead has submit96.487ms. These are CPU scopes, not GPU/compositor/shader cause or completion/FPS acceptance. Profile flag1 only; None adds no new clocks/counters/logs. Prior141421/142502 stage-upload NotExercised failures and history ResourceGrowth remain OPEN; defaultReady/performanceAccepted false. Unapplied future Disk Engine/runner fixture excluded; offline disk CPU evidence is not game-file proof. Exact artifact hashes and installed pair in GEOMETRY-CLOD-PILOT-20260927.md.

## 27 September 15:06 — actual controlled file worker and Tidewater owner preview

The merged source `68cd254b` includes all nine water commits and the additional exact safety `dec/tidewater-showcase` mission. The initial all-five-target build passed 246 scheduled steps; Rust water checks passed 61 with one explicitly ignored slow `lace_512_matches_js` test. Matched ABI17 includes water parameters1184 bytes and81 GPU timer regions. Current OP remains normal default0; Tidewater uses an explicit environment/profile selection. Lazy shore height capture removes the second full-heightmap copy from normal Current OP sessions. Minimal backend branches and the terrain wetness placeholder bind group remain; no zero-cost claim follows.

Two historical disk attempts failed and remain evidence: `disk-tidewater-20260927-20260927-150101` refused incomplete nested producer JSON (PowerShell depth2, repaired by `a547275d` depth8); `disk-complete-proof-20260927-20260927-150220` could not release its cancelled held worker because the fixture was inactive. `dddaab9b` permits only the release action while inactive. Its all-five-target18-step build passed, and the supported deployment stamped clean dddaab9b15:06. Neither failed attempt is relabelled PASS or supplied with an invented result.

Actual `disk-cancel-repaired-20260927-20260927-150656` passed/root wrapper0/cleanExittrue, with Shutdown complete15:07:10.230. The fresh external Tools producer wrote71652 bytes, SHA256 `73b0be64295f084e8051d8b9af7163c3b89810a2193956ed16426bb85d21489e`; trusted raw/derived keys and helper hash were compared independently against the controlled original Shape. No owner-thread file read or CLOD bake is claimed. File Coarse64 triangles commits after the2-triangle fallback, then Fine512 triangles commits through the existing worker. Initial coarse has2Present; fine has8Present. The held aborted job retains410190 reserved logical bytes until release/actual exit; old2Absent/jobs0 is established before replacement, so this does not claim old-active/new-fine concurrency.

The actual fine eviction reports2Present/6Absent; refill generation2 has history14,8Present/6Absent. Final history14Absent, jobs/reservations0, completed4/cancelled2, known payload138555 bytes and helper hashes unchanged. Reservations are logical bounds, not RSS or a hard I/O/bake latency bound. Pass facts remain version2/592 bytes, capabilities31/required29, with current cascade/reflection command records and cached interior/GI Pending4 or12. This does not prove all-pass pixels, nonzero survivors, queue completion, physical GPU free, retail asset integration or automatic paging.

Root handed the owner visible `tw-showcase.eden` on the same installed pair at1600x900, PID10668,15:08. The live log records Current OP -> Tidewater Native at15:08:19.599, switch#1 taking1555.1ms. This establishes startup selection only. The showcase ended clean0 at15:08:48.071; its now-final log hash is recorded below. The owner then requested a normal game with Tidewater1: PID9840 started15:10 without a test mission and logged switch#1/373.9ms15:10:08.568. That normal session remains interactive/live, so no final normal-log hash is recorded. Owner visual judgement and a matched normal-default performance screen are pending. No wave direction/breaking-quality, default no-regression, FPS or whole-map acceptance is recorded. Whole plan/StageG remain OPEN, experimental paging stays off, and defaultReady/performanceAccepted remain false. Previous ResourceGrowth and both stage-family-upload NotExercised failures remain historical gaps.

## 27 September 15:28 — optional SECTION metadata reuse and real disk faults

Source `7d4316e1` built all five targets in18 scheduled steps; Rust19 tests passed, none ignored and no adapter SKIP. The deliberate caught panic in the partial-clear transactional test is expected, not a runtime error. Clean installed stamp15:28 and actual matched pair are recorded in SECTION-SPAN-REUSE-20260927.md.

Actual disk Missing151737 and Corrupt151836 used script86615d26 with the earlier installed ddda pair; both passed/cleanExit. Real main-returned fallback remained one Present mesh with unchanged epoch/instance/helper, Failed typed Missing or Invalid, no coarse/fine payload, and jobs/reservations0. Abort then established oneAbsent/jobs0. The fault script preserves the fresh original producer file; corruption affects one byte in a private copy and missing uses a new private path. This proves controlled failure retention, not arbitrary-source freshness or pixels.

The SECTION experiment is exact startup `WGR_CULL_SECTION_REUSE=1`, default OFF. Source, resolved and material rows clear together before a retired span becomes reusable. Model births and LOD rows still append. ON-only owner8192/range4096 containers report327680 logical capacity bytes, not RSS. Cleanup is capped4096 rows/32 ranges per preparation. Partial-clear prefixes deliberately remain holes; only the completed suffix can be reused. Invalid ownership, parallel-table mismatch, mutable escape or capacity saturation sticky-disable reuse for the renderer epoch and preserve append fallback. No table shrinking, whole-ownership or physical GPU free is claimed. OFF keeps a null optional-pointer field/branches rather than zero footprint.

Actual DiskON152925 and OFF153025 passed/cleanExit/root0, each ending14Absent/jobs-reservations0. ON produced one activation, three actual postcommit reuse rows, zero disabled rows; OFF emitted no activation or commit rows. The GPU tests exercise actual compute table and material readback, source/resolved clearing, bootstrap and generational address parity, not all-pass pixels or visual equivalence.

A single same-build default-feature screen153111 passed/root0, Shutdown15:32:33.732,681 frames/drop0. It explicitly uses reuse0/water0, TerrainPageTimings1/copiedCPU1/WarmProfileOFF and other experimental feature controls, so it is not an uninstrumented normal-settings measurement. Raw milliseconds: mean52.6898649, usual median24.773, nearest-rank p95148.261/p99511.423, maximum1181.802. Quantiles use sorted index ceil(n*p)-1. Returned19923/required0/pendingfalse, geometry718731600/cap1275068416/retired0, textures2643280788 bytes/2390 records/backend5194496768. Paired token391's805.793ms surface API CPU scope is not shader/GPU/compositor cause or completion proof. Historical095 had different allocations/cache/async work; center and tail changes are mixed, with no causal no-regression or performance acceptance.

Whole streaming/StageG remain OPEN, defaultReady/performanceAccepted false. SECTION stays OFF. ResourceGrowth, stage-family upload NotExercised, complete ownership/pressure and broader gameplay gates remain. The next LOD reuse feasibility study has no implementation or runtime acceptance at this checkpoint.

Exact artifact hash manifest: [SECTION-SPAN-REUSE-20260927.md](../decisions/SECTION-SPAN-REUSE-20260927.md).

### Additional same-binary DayZ SECTION ON screen, 15:38

`section-reuse-dayz-on-20260927-disabled-20260927-153836` passed with clean exit0 and shutdown15:39:51.886 on installed7d, script/source HEADbd398893 (documentation-only after7d). Actual startup emitted one SECTION activation,64 committed reuse rows, one bounded truncation notice and no disable. This proves the optional path executes on this DayZ traversal;64 is a capped observation prefix, not total reuse. Returned19923/required0/pendingfalse. Root visually compared the two returned-POI captures and saw no obvious new missing geometry at this location; neither image is a whole-world or native-compatibility reference.

The ON screen had669 frames/drop0, mean52.2914096/median26.508/nearest-rank p95184.432/p99541.259/max992.011ms. The prior same-binary OFF screen had681 frames, mean52.6898649/median24.773/p95148.261/p99511.423/max1181.802ms. Outcomes are mixed, work and cache order differ, and the ON feature is not promoted. Actual ON geometry709926296/cap1275068416, textures2641883028/2390, backend5191781264; differences from OFF are not attributed as reclaimed memory. Both instrumented screens use water0/CPU1/WarmProfileOFF. SECTION remains defaultOFF; LOD reuse is still an unbuilt candidate at this checkpoint.

## 27 September 15:54 — compiled optional LOD payload reuse; controlled runtime passed

Source9d97c2fa built all five targets in18 scheduled steps and passed25 Rust tests with zero ignored/no adapter SKIP. The actual GPU tests include stable retired A and live B/C plus a live empty model, complete-generation mesh recreation, MODEL/LOD/material storage parity, all three main/shadow/color compute views and relative census/feedback isolation. The deliberate caught panic in the partial-clear transaction test is expected. This is not a pixel/all-pass or game-runtime result.

`WGR_CULL_LOD_REUSE=1` is an exact startup opt-in, default OFF and independently selected from SECTION reuse. An ON-only permanent zero LOD sentinel is excluded from allocation ownership/free lists. Empty births immediately use it. Retirement first compares the current range with its immutable active birth witness; only validated bounded old rows clear. The model birth is redirected to base0/count0/pad0, both MODEL and LOD uploads are marked, and retained-content epoch advances before old payload can become available. Foreign/missing/oversized witnesses tombstone without clearing another owner's rows and sticky-disable reuse. Model IDs never recycle, active LODs never relocate, current allocation/census follow relative remapped model ranges, and feedback retires the old target without assigning it to the new birth. Previously copied reports remain immutable cuts.

Owner8192 and pending/free range4096 caps remain. The4096-row/one-range cleanup limit is per retirement call, not a frame-global budget. Caps/invalid ownership/parallel mismatch/mutable escape preserve append fallback; partial-cleared prefixes remain unreused holes. This does not shrink GPU buffers or close whole-ownership/inflight/free gates. Default cost includes one additional nullable pool pointer, a startup lookup and guard plumbing; it is not zero footprint. The subsequent actual ON runtime reports container capacities below; these are logical payload accounting, not RSS.

The supported deployment installed clean9d97c2fa15:54. The attempted game fixture was refused before launch: game-lock found one external OpenPoseidon instance and the wrapper returned4. At that initial attempt game verification remained pending and no fixture result was invented; the subsequent successful runs are recorded below. No user game was stopped or overwritten. Exact build/test/refusal hashes and installed pair are in LOD-SPAN-REUSE-20260927.md. Historical SECTION notes that the LOD study was then unbuilt are historical; this checkpoint adds compile/GPU-unit evidence only.

Whole plan/StageG remain OPEN, experimental LOD/SECTION reuse stays OFF, defaultReady/performanceAccepted false. Earlier mixed default/SECTION screens, ResourceGrowth and stage-family-upload failures remain. The passing unit campaign and subsequent controlled runtime do not supply default performance acceptance.

Exact artifact manifest: [LOD-SPAN-REUSE-20260927.md](../decisions/LOD-SPAN-REUSE-20260927.md).

Actual game verification supersedes the initially blocked attempt: `disk-lod-section-reuse-on-20260927-20260927-160123` and OFF160201 passed/root0/cleanExit. ON shutdown16:01:36.603; OFF16:02:15.175. Both finish14Absent, jobs/reservations0, fineGeneration2 and helper hash unchanged. ON SECTION and LOD each emit one activation, three actual postcommit rows and zero disabled rows; each pool reports327680 logical container-capacity bytes, not allocator/RSS. Actual LOD events are birth3/base1/count1/lodLength4, birth4/base3/count1/lodLength4 and birth6/base4/count1/lodLength5. OFF has no activation/commit/disabled events. The original lock refusal remains a refused launch, not a game failure; its log is preserved.

Actual `lod-section-dayz-on-20260927-disabled-20260927-160254` passed/root0/Shutdown16:04:11.185. Both domains each emit one activation,64 postcommit rows, one capped-prefix marker and zero disabled rows. This proves execution on the DayZ route, not total reuse/whole-table convergence.652 frames/drop0: raw mean55.0970475460ms, usual median24.6635, nearest-rank p95189.194/p99525.165/max899.061; quantiles use sorted index ceil(n*p)-1. Returned19923/required0/pendingfalse, geometry725795512/cap1275068416/retired0, textures2798329684 bytes/2529/backend5348500944. ModeDisabled uses water0/bothreuse1/TerrainPageTimings1/copiedCPU1/WarmProfileOFF; it is an instrumented feature screen, not normal untouched settings. The earlier7d SECTION-only route has different allocations/cache/async work and source, so there is no isolated default or causal performance control. No promotion/no-regression acceptance follows from mixed center/tail observations. Root compared ONboth and the historical7d default-OFF returned image at this POI: no obvious new missing geometry locally. The captures are not pixel-identical or whole-world/native-compatibility truth; no broad visual acceptance follows.


## 27 September16:52 — actual operation-local cold primary PAA upload

Source 5b57f097 installed clean at16:52. The optional operation-local cold PAA handoff passed all-five-target build202 steps and187 assertions/3 ON cases without skips; earlier e749 build19 steps, ON122/3 and OFF208/9 passed. Actual original two-model ON165226 and OFF165339 passed with normal exit0, both resident2/required0/pendingfalse and one normal vertex buffer per object. ON recorded one unique source-validated successful5440-byte block upload; OFF recorded none and retained no archive bindings. The archive contains five mips/5456 decoded bytes, but existing Init selects four/5440. Source-bound converted ownership survives global-store absence; one owner admission claim rechecks physical member, bank header and cancellation token. At most one eligible visual primary per conversion,64LOD/256 visits, workers<=2; source/retained chain each16MiB, seven selected mips, optional ready128MiB. These are logical capacities, not RSS or a hard LZO/worker deadline. Earlier compile failures, null-source unit failures, destination-format mismatch and e749 game NotExercised remain recorded. Parsed MLOD quads now qualify only for the cold dependency gate; global Mesh::HasGeometry and normal admission policy are unchanged. Root viewed opaque red boxes in ON/OFF captures without an obvious local appearance difference. This is actual primary-upload and local admission proof, not material-stage/all-texture readiness, full assets, pixels/all passes, GPU completion, FPS or no-regression acceptance. Exact flag WGR_OBJECT_STREAM_COLD_TEXTURE_HANDOFF=1 remains OFF by default; whole plan OPEN and performanceAccepted/defaultReady false. Details and hashes: COLD-PAA-HANDOFF-20260927.md.


## 27 September17:04 — optional bounded MODEL source-row uploads

Compiled source77b50518, supported clean stamp31c80f9c17:04 (doc-only commit after compiled source), passed all-five-target build18 steps and27 Rust tests without ignored/adapter skips. Exact startup WGR_CULL_MODEL_ROW_UPLOAD=1 remains OFF by default and independent of SECTION/LOD. A fixed minimum/maximum dirty interval covers model births, changed feedback tags and actual LOD-sentinel tombstones; missing/grown storage and unknown intervals upload the full mirror. Reset follows all successful selected-table writes, preserving dirty retry state on failure. Actual64-byte Option metadata/startup guard/two None getter branches remain evenOFF, not zero footprint. DiskON170511 and OFF170654 passed normal exits with final14Absent/jobs-reservations0/helpers unchanged/fineGeneration2. ON recorded activation1 and8actual partial write notices (seven16-byte writes, then64bytes for4rows); SECTION/LOD each3commits. OFF all three domains emitted no notices. MODEL-only DayZON171446/OFF171306 passed/drop0; ON825frames mean43.445720 median24.465 nearestp95126.106/p99335.488/max1029.541, OFF788 mean44.147617 median23.45/p95137.387/p99443.227/max1019.220. ON median/max worsen while other summaries improve, with unequal transient coverage and geometry/texture work. Both returned19923/required0/pendingfalse; geometryON710379372 vsOFF717813696, texturesON2749355764/2489 vsOFF2778451656/2525, cap1275068416/retired0 both. Source-write notices are limited to16 plus one cap marker, not total route writes, driver completion or a frame/global budget. Terrain/copiedCPU diagnosticsON/WarmProfileOFF; ordered cache/work differences forbid causal FPS/no-regression acceptance. Whole plan OPEN/defaultReady-performancefalse; no default promotion, ownership/free/all-pass closure. Precise evidence and hashes: MODEL-ROW-UPLOAD-20260927.md.


## 27 September 17:35 — controlled original-MLOD offline producer

Source 38765117 adds an optional offline original-P3DM producer, not a game consumer. Repaired all-five-target builds passed 435 and 13 scheduled steps; focused actual export/packing/producer checks passed 314 assertions in 14 cases without skips. Initial friend-access compile failure and live-TestApplication unit assumption failure remain historical. Actual input 24,136 bytes produced selected package 70,084 bytes, authored fallback 2/coarse 63/fine 512 triangles; invalid trailing byte was refused with exit 2 and no output. Supported clean installation at 17:35 passed disabled ordinary-MLOD smoke with exit 0, normal shutdown, no forced termination and two admitted red boxes; local inspected appearance showed no obvious change. It did not consume the new package. The controlled untextured two-visual-LOD subset retains assumed original winding. Owned input cap16 MiB and serialized cap128 KiB are logical bounds, not upstream CLOD scratch/time or RSS guarantees. Ordinary controlled=false conversion retains policy with a small false branch/null-context guard cost. No runtime/default paging, new live bake or world asset path is activated. Whole plan OPEN/defaultReady-performanceAccepted false. Details: CONTROLLED-MLOD-PRODUCER-20260927.md.


## 27 September 18:04 — bounded original-source startup admission

Source 9d82ba05 adds copied actual neutral ShapeSection material and an outward-rounded origin sphere to the controlled original-MLOD DTO, plus explicit startup source admission; e67a1842 repairs negative-startup cleanup using the exact normal ShutdownRuntime body. All-five-target builds passed 262 and19 steps, focused tests442 assertions/21 cases without skips. Four repaired same-pair modes passed: Valid admitted epoch1/raw24136/known32240/snapshotValidated; HashMismatch typed7/exit2; DisabledArguments explicit opt-in refusal/exit2; DisabledNoArguments no admission/exit0. All had verified normal shutdown and no forced termination. Initial Valid passed, but initial mismatch failed its shutdown-marker gate despite typed7/exit2; its normalShutdown=true artifact flag is inconsistent and not cleanup proof. Clean supported e67 installation18:04 was used for all repaired modes. Valid/default pictures show the same two ordinary opaque red boxes locally; admitted original geometry was NOT rendered. The retained32240 bytes charge one copied source record and vector capacities, excluding temporary parse/bake, allocator/kernel/RSS. The default fixed configuration strings, engine pointer and startup checks have a nonzero footprint; no default source IO/export/state is performed. Exact WGR_GEOMETRY_PAGE_ORIGINAL_SOURCE=1 remains OFF. Renderer Begin remains Unsupported and future consumer code is excluded. Whole plan OPEN/defaultReady-performanceAccepted false; no FPS/all-pass/source-freshness/global-free claims. Details: ORIGINAL-GEOMETRY-ADMISSION-20260927.md.


## 27 September 18:25 — actual original-file private paging consumer

Compiled consumer3f96252d, installed clean doc-only stamp93174efa18:25, passed all-five-target build249 steps and6196 assertions/35 cases without skips. Runner89ca4c83 repairs nested JSON depth12 after an initial strict-key API refusal with normal exit0; the failed fullcycle remains historical. Actual original-file fullcycle, Missing, Corrupt, syntheticSOURCE0 and ordinary defaultOFF controls passed normal exits. One source admission epoch1/32240 logical bytes is consumed once. Real original fallback2/coarse63/fine512 use copied actual materials and origin sphere2.828427314758301; no invented CPU helpers (count0/emptyhash/cpuHelpersUnchangedfalse). Fullcycle held cancelled Fine retained410238 scheduling bytes until actual release; fresh Fine yielded8Present records, one eviction2Present/6Absent, generation2 refill8Present/6Absent across14history, final14Absent/jobs-reservations0, secondBegin Busy/Consumed. Known fixture bytes138573 include admission once, but the engine retains that source through process exit; no fullRAM/GPU-free proof. Missing/Corrupt typed failures kept exactly1fallback/unchangedinstance then1Absent/jobs-debt0. Synthetic retained3helpers/hashunchanged/final14Absent. DefaultOFF made no source admission and preserved two ordinary objects. Root saw a black curved fine silhouette; the low-angle authored plane fallback was not discernible, so material/winding/visiblefallback quality is NOT accepted. Copied pass factsv2/592/cap31 are actual command-recording only, with cached Pending bits allowed. Exact source/fixture flags remain OFF; no automatic demand, screen-error/view-union frontier, repeated cycle, retail integration or performance acceptance. Whole plan OPEN/defaultReady-performanceAccepted false. Details: ORIGINAL-GEOMETRY-PAGES-20260927.md.


## 27 September 18:54 — actual private camera preparation with corrected source material

Source fixture21186ff5/5ca22306 fixes only authored original orientation, preserving global loader/MeshBuild: upright fullcycle184229 passed with raw24136 C7C5694C...77F0/selected70112 B16234E1...082C and64/512 cuts, build19/test11322 assertions19cases PASS. Initial low-angle fallback invisibility and black fine silhouettes remain historical quality failures. Source92c630af adds exact ordinary identity layer-color values to plain source material and explicit private camera prefetch; all-five build250/test16766 assertions20cases PASS/noSKIP. Same-pair camera185438, manual185700 and default185930 passed normalexit0. Camera far120.037 produced no request; near16.279 heldrequest1/debt410222; departurecancel1 retained active debt until real release. Reentryrequest2 prepared8Present records with finePrepared=true/fineSelected=false and unchanged coarse model/instance; manualFine reused handles/completed2. Stop removes64-byte policy charge; Abort8Absent/jobs-debt0 and secondBeginConsumed. Manual regression final14Absent/gen2/debt0; default noadmission/twoordinaryVB/required0. Root saw visible white flat fallback and curved fine locally, not all-pass/material/retail quality acceptance. Exact source/fixture gates and explicitfollowCamera remain opt-in; default NextFrame pointer/optional-pointer branch and report fields cost remain, no literalzerofootprint. Distance/dwell/hysteresis predicts preparation only, no projectederror/viewunion/fine selection/automaticeviction. Admission persists through engineexit; logical stored charges do not prove RSS/devicefree/performance. Whole plan OPEN/defaultReady-performanceAccepted false. Details: ORIGINAL-CAMERA-PREFETCH-20260927.md.

### Conventional occupied-prefix copy and optional cold alpha — completed 27 September checkpoint

Conventional dbe68b03 installed19:21: all-five build203 PASS; repaired pool17 tests PASS/noSKIP after historical wgpu29 ErrorScope API compileFAIL. Initial cold ON all6 SKIP/exit4 with workers2 is not PASS; worker1 ON563assert6cases and OFF2assert1case PASS/noSKIP. Default occupied-prefix copies retain capacity, offsets, interior holes and epochs; actual DayZ192612 passed with12 contiguous notices, nominal1255145472/copied1252122152/saved3023320 bytes (bounded observed prefix, not route totals or capacity release). Instrumented624-frame92 baseline mean58.363010/median26.5195/p95185.105/p99511.312/max1062.104 versus760-frame dbe mean45.478966/median25.9735/p95144.786/p99490.696/max904.133 has unequal async allocations/cache/work and new diagnostics: no causal FPS/no-regression acceptance. Returned19923/required0/pendingfalse; pool live710591632/cap1275068416/retired0, textures2749224436/count2488/backend5301730128. Untagged cold192150 failed strict NotExercised because source alpha FLAG absent; BC3 alone does not force classification. Script fixture ac32771e/81c073b2 adds original alpha tag; same installed dbe taggedON192902 PASS peek1/baseline0 plus actual5440B source-validated operation-local upload; taggedOFF193109 PASS peek0, both ordinary2VB/required0/normalexit0/no force. Cold path borrows top blocks without consuming chain, rechecks source/header/token after visitor and retains subsequent once-only upload. Existing reader fallback, optional cold flagOFF, cached branch/TLS-scalar footprint retained. Narrow root local images show no obvious new missing geometry; no all-map/material/all-pass or performance acceptance. Whole plan OPEN/defaultReady-performanceAccepted false. Details: CONVENTIONAL-COPY-COLD-ALPHA-20260927.md.

The exact completed artifact manifest and scope are recorded in [CONVENTIONAL-COPY-COLD-ALPHA-20260927.md](decisions/CONVENTIONAL-COPY-COLD-ALPHA-20260927.md). This checkpoint adds no geometry-page selection, pressure eviction, or automatic camera frontier contract.

### Real DayZ initial-admission alpha reuse — completed 27 September checkpoint

Actual DayZ startup owned-alpha checkpoint uses unchanged installed dbe68b03/19:21 pair with additive script c822b64a startup gate. Initial194013 FAILED omitted archive-binding expected flag and finally forced its owned test process/no normal shutdown/result; repaired194414 still FAILED strict route NotExercised despite normalexit0: first0/resident0/pendingtrue -> settledbaseline6/resident19923 -> returned6, routeDelta0 and16truncated validupload witnesses. Preserve both failures. StartupON195430 PASS707frames/drop0 first0 -> settled6/19923, positiveinitialAdmissionDelta6; strictroutepositivefalse/delta0 remains explicit. OFF195215 PASS784/drop0 allpeeks0/no cold upload, sameinstalledhashes. Bothreturned19923/required0/pendingfalse/normalexit0; ONlive716514504/textures2809208052/count2530 versusOFF711584488/2763029844/count2502, samecap1275068416. Quantiles nearest-rank/medianusual; orderedcache/unequalwork/ONarchive-service+diagnostics prevents causalFPS/no-regacceptance. Scalar six classification commits and16 upload observations are separate witnesses, not same-asset identification/fullroute/wholematerial readiness. Existing source/read caps and source maxlevels7 unchanged; coldflag remainsOFF. Rootsettledlocalbus-kiosk-building images compatible withoutobviousmissinggeometry onlylocalnotpixel/allpass/worldtruth. WholeplanOPEN/defaultReady-performanceAccepted false. Details: DAYZ-COLD-STARTUP-ALPHA-20260927.md; no more runtime required for this narrow phase, next parked diagnostic remains pending.

See the [completed source and artifact evidence](../decisions/DAYZ-COLD-STARTUP-ALPHA-20260927.md). Startup success does not replace the unchanged independent route requirement.

### Bounded parked geometry attribution — completed 27 September checkpoint

Parked report662130e3 cleaninstalled20:11 all-five252buildPASS/focused9892assert24casesPASS/noSKIP. ExactoptionalWGR_GEOMETRY_PARKED_REPORT1 plusownerledger; bounded256models/2048capturevisits and2048indexedmodelLODtuplevisits, one SnapshotHandles mailbox drain and existing Rustfacts call. Earlierproducerzero-ref selection is not currentparked-state/exclusiveownership/reclaim proof. ActualON201145 threecycles PASSConverged/lateQuiet/retiredClear/positivecandidateusage with unchanged ResourceGrowth gate; return2live729236116→return3729519052 growth282936<=8MiB, cap1275068416 unchanged/retired0;texturecount2495→2517 growth22<=25. Request1 captured46models144alloc36975596B=CPUborrowed18532708/71+unborrowed18442888/73. Requests2/3/4 captured54models164alloc41568568B=CPUborrowed18532708/71+unborrowed23035860/93; alljoincomplete/outside-unknown-absent0/truncfalse. Knownrequest2020/link3376 capacities excludeallocator/transientjoins/RSS. Finalborrowed44.58% of selectedparkedset, notwholeCPU84.5%; unborrowed23MB~3.16% live remains onlycandidatecategory. OFF201522 AllocationDiagnosticOnly PASSzero traversalcycles/allparkedfields0/no capturetupleallocation; notsame3cycleperformancecontrol. ownershipCoverageCompletefalse/reclaim0 unchanged; prior+15338464 ResourceGrowthFAIL preserved. No hardbytepressurepolicy/netrebuildtradeoff/performanceacceptance/paging or defaultpromotion. WholeplanOPEN/defaultReady-performanceAcceptedfalse; detailsPARKED-GEOMETRY-REPORT-20260927.md.

See [PARKED-GEOMETRY-REPORT-20260927.md](../decisions/PARKED-GEOMETRY-REPORT-20260927.md). This diagnostic selects no pressure policy and grants no physical reclamation or default-readiness acceptance.

### Loose-PAA single native capture — completed 27 September checkpoint

Loose singlecapture f9e87d41 cleaninstalled20:30 all-five22buildPASS/actualfile81assert5casesPASS/noSKIP. Corrected0C587 source only; older static drafts notexecuted. DefaultON looseworker uses one native stream/captured buffer for BC then RGBA fallback, productionSIZE_MAX preserves legacyeligiblebytes/mipcounts and unchanged globaldecoders; helper16MiB/32level policy unusedproduction. RawmalformedTAG/OFFS/payloadrefusal losesoptionalpreparation only/fullownerfallback, bounds/freshness/cancel theoremOPEN. ActualDayZ203013 PASS791drop0/normalexit0/Shutdowncomplete20:31:24.806 returned19923desired19923/required0/pendingfalse/coldpeek0;live702520576/cap1275068416/retired0/textures2656081556/count2394/backend5207312384. Worker final0chains/0MB/173ms unreadable8634/skipped3137/consumed0of2764 provesactualnegative loosemiss stage, not readableBC/RGBAretailpositive; nativefileunits cover acceptedbytes. Earlier195215oldinstalleddbe workerunreadable8654/283ms differswork/cache/order, no causalworker/FPS/no-regacceptance. Counters cumulativeworkerCPU notsyscalls/uniqueassets/perrouteGPUclocks. Rootlocalsettledbus-kiosk-building-tree view noobviousmissingnewgeometry onlynarrowcompatibility. No newcache/flag/capacity/free/defaultpromotion; wholeplanOPEN/defaultReady-performanceAcceptedfalse. Exactmanifest LOOSE-PAA-SINGLE-CAPTURE-20260927.md. Nextofflinegeometrycertificate unbuilt/pending excluded; no further smoke needed for this narrowphase.

See the [completed compatibility and runtime evidence](../decisions/LOOSE-PAA-SINGLE-CAPTURE-20260927.md). Production source/output memory bounds remain open.

### Optional offline selected-surface certificate — completed checkpoint

Offline selected-surface certificate5957e289: all-five repaired builds PASS; initial helper compile FAILED ambiguous Vertex/Vector3P retained. Helper19736 assertions/9 cases and final CLI19916/12 PASS without skips. Optional --surface-certificate remains OFF; no renderer/game invocation, installed f9 unchanged. Actual OFF/ON selected70112 bytes SHA b16234e1327a6acbd00b42999bf5b1d64b0aa0d5cb5b6d3efc44dd2beaaa082c and manifest1581 bytes SHA d0d36ce17faf733c6e739bc1123d54ecd276b8bffb4a3eb00376a68bf7be1eb6 are identical; OFF emits no descriptor. Actual raw24136 bytes c7c5694c700286300af559d98212017dbbc028206a5ca6c9f9af1f6905bc77f0, selected coarse64/fine512. Root verifier PASS full identities/thresholds/cut IDs/packed hashes/exact double bits and 240 spot samples each direction; samples are not the continuous proof. Reviewed convex-anchor/outward-interval construction bounds complete selected triangle unions: fine-to-coarse1.1853434305796802, coarse-to-fine and symmetric4.0000000000000027;41600 distance visits under2M. Deliberately loose even identical surfaces; not original fidelity/pixels/UV/normals/topology/projection/all-pass/runtime selection. Private optional successful-producer seal binds exact encoded bytes and manifest; canonical re-encode checked before directory creation, transaction rollback and actual invalid-tail CLI exit2/no artifacts. Actual Tools compiled dirty09adb3e0 source matches committed595; no new game smoke warranted for unused offline path. Full plan OPEN/defaultReady-performanceAcceptedfalse; future warm preflight source-only excluded. Exact artifact manifest SELECTED-SURFACE-CERTIFICATE-20260927.md.

See the [offline certificate evidence](../decisions/SELECTED-SURFACE-CERTIFICATE-20260927.md). This prerequisite does not enable automatic geometry selection.

### Warm material preflight — completed failed-positive checkpoint

Warm material preflight16ff6aaa cleaninstalled21:11: all-five initial250/clean18 builds PASS. Initial77 assertions/five PASS/three SKIP plus unmatched warm-texture-jobs is partial; mixed1408/15 had13PASS/twoFAIL/four assertions from old archive-outsideScope expectations under jobs1, no source fix. Corrected fresh1116/12 and separate jobsOFF archive317/3 PASS/noSKIP. Actual strict ON211225 FAILED NotExercised despite normalexit0/Shutdown21:13:41.015; five missing road_SM header Loads returnedtrue/nonthrow but acceptedfalse, no same-source upload. Source cap256/live256/init-wrapped256/capRefused4098 is cumulative, not per-source cause. Matched warm-jobs/profile ON preflightOFF211431 also FAILED existing generic upload gate, normalexit0/Shutdown21:15:43.609, no preflight rows. Both1completed job/1member/1Put/0Take/0Upload; queue-active-jobmetadata-scratch0 but optional queue1056/store1entry1573776 retained. Both19923desired19923/required0/pendingfalse/cap1275068416/retired0; work differs. ON749frames mean45.933695594/median23.812/p95142.571/p99477.233/max845.967 versus OFF836/42.325684211/23.804/122.561/353.920/1221.295; nearest-rank tails, mixed ordered profiled unequal work forbid causal FPS/no-reg acceptance. Root both settled bus-kiosk-street-trees-buildings locally compatible only. Two failure-charged attempts/recentre/shared256 visits, actual Init/liveRef/member proof unchanged, synchronous Load not timebudget. Exact startup flag OFF; no stage material/pixel/readiness/performance proof or promotion. Wholeplan OPEN/defaultReady-performanceAcceptedfalse. Future RVMAT/source-lifetime and projected helper unbuilt excluded. Evidence WARM-MATERIAL-PREFLIGHT-20260927.md.

See [preserved failed gates and bounded scope](../decisions/WARM-MATERIAL-PREFLIGHT-20260927.md). The experiment remains OFF.

### Pure projected selected-surface bound — completed prerequisite

Pure projected-surface prerequisite24283a81: all-five build PASS with changing schedule, focused13900 assertions/11cases PASS/noSKIP. Unused fixed-value header and tests only; no Game/ToolsCLI/camera capture integration, no deploy/new smoke, installed16ff unchanged. Caller must supply immutable successful certificate/fullkeys/hashes/cuts/threshold/errorbits, full both-union enclosing AABB and one actual joined camera-relative model/view/projection/viewport/near tuple with matching generations. Outward interval affine composition and row-L1 quotient sensitivity include offaxis depth/nonuniform transforms; wholebox near intersection or lateral clipping refuses transactionally. IEEE normal-or-zero/domain/fastmath/underflow endpoint refusal; fixed arithmetic/no scans/allocations. Tests use synthetic policy authority plus actual ConvertMatrix/ConvertProjectionMatrix and manual existing WGPUdepthoverride; independent longdouble equation samples are not guaranteed higherprecision onWindows. Not raster/shaderfloat/visibility/pixel/allpass/retail/normal-UV/topology/originalfidelity or runtime selection/performance acceptance. Prior actualoffline4unit bound deliberatelyloose and not connected runtime projection. Wholeplan OPEN/defaultReady-performanceAcceptedfalse; warm failed upload gates retained, next startupsidecar draft unbuilt excluded. Manifest PROJECTED-SURFACE-BOUND-20260927.md.

See [caller contracts and executed CPU checks](../decisions/PROJECTED-SURFACE-BOUND-20260927.md). Automatic selection remains unimplemented.

### Verified surface admission; hot proof retirement upload gate remains failed

cc0580ca optional surface admission and hot PAC proof retirement checkpoint: all-five builds PASS after retained Verify-macro and Game meshoptimizer-include failures; focused16298 assertions/17 cases PASS without skips. Clean installed22:05. Actual valid surface/camera lifecycle PASS with920-byte numeric proof/33192-byte retained admission; malformed sidecar PASS manual fallback/proof0, corrupt selected file PASS typed worker refusal/no verified cut. Hot ON and OFF actual DayZ strict upload gates FAILED NotExercised despite clean exit0: ON64 owner drops/4 observed debt decreases but no later successful stage-family upload; OFF0 drops; both1Put/0Take/0Upload. Cap256 and alias debt remain. Both19923 returned/required0; unequal allocation/cache/profile work excludes causal FPS/no-regression acceptance. Optional flags OFF; wholeplanOPEN/defaultReadyfalse/performanceAcceptedfalse. Exact21-source snapshots and verified115-artifact manifest build/surface-hot-artifact-manifest-20260927.json; see SURFACE-ADMISSION-HOT-PROOF-20260927.md.

### Tidewater W6 integration: paired startup checks, wider acceptance remains open

Tidewater W6 integration14f323cf includes full requested source3e99fee6 ancestry (27 new/9 equivalent commits), preserving CurrentOP backend0 and lazy shore capture. MatchedABI18 appends water1216 bytes/offsets1184,1200; CPU72/pass592 unchanged. Buildhead8153881b all-five254stepsPASS; Rustwater44PASS/1ignored slowdebuglace, ABI21PASS, C++4119assert13PASS/noSKIP. Supported installed81522:26. Actual root-owned backend0 and1 showcase startup/capture bothPASS cleanexit0 unchangedpair; OFFnoTWinit manually verified, ONswitch/native shader creation without errors. Root localcoast/LST images differ in water appearance; no quality/performance/underwater/wreck/network acceptance. Global vehicle/flood/camera/crater/cloud behavior changes also affect backend0; bounded default packing and persistent wreck work are real costs. Camera/weak-cache candidates excluded. WholeplanOPEN/defaultReadyfalse/performanceAcceptedfalse; verified64-artifact manifest and48exact14f snapshots; TW-WATER-13-INTEGRATION-20260927.md.

### Camera authority checkpoint: native passed; remaining runtime gates open

Camera/weak checkpoint cdef7c74: all five targets PASS after three preserved compile failures (Harness numeric macro scope, slice len, guessed count); final build scheduled18. Fresh C++ weak ON62/2, OFF18/1, shared budget16/1 and projected helper4975/4 PASS without skips; Rust main-camera5 and ABI21 PASS/0ignored. Installed cdef7c74 23:12 matched pair. Native camera lifecycle PASS with actual v1/192, fresh moved bound, near/clipped refusal, refill and final14 Absent/jobs0/debt0. Scaled arm FAILED strict dimensions despite clean exit0; actual1280x720 was not claimed as960x540. Runner d4e5653b live-scale repair is unexecuted; OFF control refused the game lock, weak DayZ runtime PENDING. Pure preparation policy0fc9db57 is unbuilt/unexecuted and not installed. All new gates stay OFF by default; no automatic cut/pixel/all-pass/GPU-free/FPS acceptance. Full plan OPEN, defaultReady=false, performanceAccepted=false. See CAMERA-WEAK-CACHE-20260927.md and build/camera-weak-checkpoint-artifact-manifest-20260927.json.

### Tidewater W6g and requested new default: verification pending

Tidewater W6e-W6g source merged64fd11ba from exact requested e10f5e74 branch (three missing commits); new Tidewater-default nine-path adaptation e4d32224 source-reviewed but unbuilt/unexecuted at this checkpoint. Fresh/absent profile/unknown backend now intends1, explicit saved/UI0 and exact env0 preserve legacy, unsupported storage falls back. Constructor allocates only chosen startup backend; saved0 without env may still switch from initially constructed TW after first producer. Real additional car/hull CPU work and changed default resource costs have no performance acceptance. No ABI/layout changes (18/1216); streamer camera/pass/CPU work preserved. Installed cdef23:12 remains previous camera build, not this water candidate. New build/tests/deploy/default/legacy/vehicle runtime all PENDING; full plan OPEN/defaultReady=false/performanceAccepted=false. See TW-WATER-14-PENDING-20260927.md.

### Actual warm prepared-texture provenance

Warm texture provenance893b48b9: all-five build602 PASS; fresh trace44 assertions/4 cases and weak-cache ON80/2, OFF23/1 PASS without skips after an initial unit-runner attempt omitted the required hot-retirement flag and failed2 cases. Supported installed ed40abd4 stamp00:34 is compiled893 plus docs-only head. Actual DayZ `warm-provenance-on-20260928-disabled-20260928-003441` FAILED its unchanged strict useful-upload gate (result.passed=false), though owned exit0, normal shutdown and no forced termination were retained separately. Two immutable exact-member tokens were observed within the shared2MiB source-metadata cap: token1 owner exact attempt and key-only store miss preceded an ordinary1,398,016-byte upload; its worker started and Put later. Token2 worker Put succeeded about2.79s before its owner exact attempt, yet the store Take was a key-only miss and the owner used ordinary5,592,064-byte upload. Aggregate warm Puts2/Takes0/Uploads0, one prepared-PAA cancellation and one final store entry do not identify why token2 disappeared; configured TTL defaults30s and was not overridden. No per-token erase/reset reason is recorded, so cancellation is a hypothesis, not a finding. Cumulative weak wrappers367 and first Init handoffs367 are positive while weak live cells0 at the final cut; trace2 sources/16 rows/no truncation/9184 known bytes, cap256 strong and2MiB total unchanged. Source-token rows are a bounded diagnostic, not whole-route/member coverage, GPU completion or a causal FPS/no-regression proof. Experimental flags remain OFF by default; whole plan OPEN, defaultReady=false, performanceAccepted=false. See WARM-TEXTURE-PROVENANCE-20260928.md and build/warm-provenance-artifact-manifest-20260928.json.

### Actual bounded projected preparation lifecycle

Projected private preparation follow-up: actual source d1696c76 with runner-only serial repair80821975, installed matched bfe83b9e stamp00:16, passed `projected-prefetch-serial-20260928-20260928-002219-531dcf` with owned exit0, normal shutdown and no forced termination. The copied successful same-frame main-camera position [1200,100000,1240] yielded certified ideal selected-union upper0.0558372136760545 (below unchanged leave rail1); fresh near camera yielded upper459.322014740524 and one bounded Fine request. Departure cancelled it while one job and410238 logical reservation bytes remained until worker exit; after drain a second request produced8 Present Fine records and finePrepared=true while the current coarse instance stayed selected. Explicit manual selection then chose Fine; Abort ended8 Absent, zero jobs and zero reservation debt. This validates one private preparation/cancellation/reuse lifecycle, not automatic cut selection, visibility, shader/pixel/all-pass equivalence, retail assets, physical GPU release or performance. The earlier10-km and clamped out-of-world100-km strict failures remain separate historical evidence; the latter returned OK for the requested pose but the Zeus camera was terrain-constrained. All experimental flags remain OFF by default; full plan OPEN/defaultReady=false/performanceAccepted=false. Evidence hashes: build/projected-success-artifact-manifest-20260928.json.

### Follow-up: projected far-position refusal and weak accounting tests

Follow-up 2026-09-28: source232f4ec7 cumulative weak-cell accounting built all five targets (210 steps); focused weak ON80 assertions/2 cases, OFF23/1 and shared-budget16/1 all PASS without skips. These unit checks do not convert the earlier strict DayZ weak gates into passes. Source55e5c0a8 widens only the certified model translation input to 1e6 while basis/view/projection/geometry remain at 1e4; all-five build19 and projected math7453 assertions/6 cases PASS. Supported installed55e5 at00:04. Actual 100-km projected-prefetch retry FAILED its unchanged far-before-activation rail: valid generation74 ideal upper4.58863291149517 > leave1, runner passed=false with owned exit0/normal shutdown/no forced termination. The requested x=-98800 pose returned OK, but CameraHold::Simulate clamps Zeus x/z to the loaded terrain and engine.log recorded the boundary warning; OK did not certify the captured position. The failed report did not include camera XYZ, so exact captured depth is unknown; no projected prefetch runtime PASS or math defect follows. A separate same-frame camera-position/vertical-pose diagnostic is source-only and unbuilt at this checkpoint. No rail or acceptance threshold changed. Full plan OPEN/defaultReady=false/performanceAccepted=false. See build/projected-boundary-amendment-manifest-20260928.json.

### Actual feef water default and camera controls; projected/weak strict failures

Actual feef4140 checkpoint: all5 build253 PASS; projected/camera854assert7, Water/ship4119assert13, weak ON62/2 OFF18/1 PASS/noSKIP; Rust backend4 and ABI21 PASS/0ignored. Installed feef23:36 matched pair. New Tidewater default(noenv) and explicit0 legacy startup PASS/clean0; imported3 W6e-W6g vehicle changes remain runtime-unverified. Live scaled v1/192 actual960x540/output1280x720/status5 and OFF manual14-Absent control PASS; original scaled failure retained. Projected-prefetch strict far bound FAILED before activation (actual4.588616>leave1, clean0); no preparation runtime PASS. Weak ON FAILED early zero-live gate with retained observations/no original result; OFF FAILED strict later-upload gate despite4 logical owner debt drops,1Put/0Take/0Upload and clean shutdown. Default source/policy scopes distinct; no cap/threshold/criterion relaxation. Future cumulative weak diagnostics and projected pose correction are unbuilt/unexecuted and excluded. Full plan OPEN/defaultReady=false/performanceAccepted=false; see updated TW-WATER-14-PENDING-20260927.md and build/feef-projected-water-weak-artifact-manifest-20260927.json.

### Bounded warm-store retirement reasons: one-token strict failure

Warm retirement reason 4da72b00: all-five build24 PASS; fresh pure trace53 assertions/5 cases, actual source-bound store23/1 and existing warm-store32/1 PASS without skips; broad PAA432 assertions/8 PASS plus one explicit TTL=.01 skip. Supported installed 4da72b00 at00:56, matched EXE823744247C20ECFD2524D07E2E22AAFEC7D37B834B99B56EF015354349EDF545 and DLL C85EB765FFBF0302029757B5D00A22E4A7DD083B862C91BF0D616AA8174768C3. Actual DayZ warm-retirement-reason-20260928-disabled-20260928-005629 FAILED unchanged strict later source-validated worker-stage upload gate (result.passed=false), with owned exit0, normal shutdown and no force. Only token1 was admitted: exact owner attempt/key-only store miss at00:57:02.317 and ordinary1398016-byte upload at02.321 preceded worker start02.712 and StorePut02.714. Trace1 source/8 rows/no truncation, no storeRetired reason row; one final prepared entry1573776 bytes and zero prepared cancellation. This cannot diagnose the earlier two-token run's token2 disappearance, prove the final entry's identity or establish useful warm upload. New reason bits mark actual stored exact-member erasures but coalesce repeats per token and emit later outside the store lock; UploadedMark is key-based removal, not same-member uploader proof. The trace is opt-in under existing three flags,8 sources/128 rows/four per event, charged9248 known C++ bytes inside the unchanged2MiB metadata budget; off-path still has a small warm-entry branch. No matched default run, causal FPS/no-regression, physical release, readiness or promotion. Whole plan OPEN/defaultReady=false/performanceAccepted=false. See WARM-TEXTURE-RETIREMENT-REASONS-20260928.md and build/warm-retirement-reason-artifact-manifest-20260928.json.

Warm first-member slice 552bd67d: all-five build25 and fresh policy910 assertions/4 cases, default-off7/1, actual worker119/4 PASS without skips; clean installed01:14. Actual DayZ011523 FAILED unchanged strict later stage-family upload gate despite owned exit0/normal shutdown/no force. Exact token1 early worker started with214 cold queued, StorePut then source-validated StoreTake and successful1398016-byte upload; two completed warm jobs/2Puts/1Take/1Upload. Token2 StorePut was retired Cancelled ~100ms before its exact owner key-only miss and ordinary5592064-byte upload. The recenter is consistent with model-publication invalidation but lacks exact model/epoch witness. No pending/active jobs at final cut; returned19923/required0. One profiled run, unequal work and no matched default screen forbid causal FPS/cold-throughput/no-regression acceptance. Candidate flag OFF; useful single-member upload is not full stage/material readiness, GPU completion or physical memory release. Whole plan OPEN/defaultReady=false/performanceAccepted=false; see WARM-FIRST-MEMBER-SLICE-20260928.md and build/warm-first-slice-artifact-manifest-20260928.json.

Warm published reuse c803fa3c: final all-five build41 PASS and fresh actual-store145 assertions/2 cases, preparerON13/1 OFF11/1, default7/1, existing actualworker119/4 PASS/noSKIP. Clean installed01:34. Actual DayZ013439 requested promotion but observed0 rows; one token early-selected with214 cold queued, StorePut, exact source-validated Take and1398016-byte upload. Counters1job/1Put/1Take/1Upload/0cancel; no queued/active jobs. The unchanged strict later owner-drop/stage-family gate FAILED NotExercised despite owned cleanexit0, so this run does not test the new transfer or resolve prior token2 cancellation. Bounded32-entry prefix can miss candidates; source identity and later Take/upload remain separate witnesses. One profiled ordered run with unequal allocated work is not causal FPS/no-regression or full-stage readiness proof. NewflagOFF/defaultReady=false/performanceAccepted=false/wholeplanOPEN; see WARM-PUBLISHED-REUSE-20260928.md and build/warm-published-reuse-artifact-manifest-20260928.json.

Warm stage refusal diagnostics e8ade72a: all-five build22 PASS. Initial combined test invocation was configuration-incompatible (6/8 cases,395/399 assertions; two archive-cap cases failed); correct fresh-env archive actual361/4, model-purpose48/2 and preflight20/2 PASS/noSKIP. Clean installed01:52. Actual DayZ015252 result.passed=true ONLY because strict later stage upload was not requested; owned exit0/normal shutdown/no force. Five `_sm.paa` attempts returned/usable but refused NoInitializedBinding/currentSource=false/accepted=false; no same-source stage upload. Each row globalCapacityRefusedDelta1 is concurrent/global, not a per-texture cap cause; final 256 live bindings/4144 capacity refusals do not justify cap promotion. Separate primary warm path 1job/1Put/1Take/1source-validated1398016-byte upload passed, not material-stage readiness. Returned19923/required0; profiled route and unequal work give no causal FPS/no-regression proof. FlagsOFF/defaultReady=false/performanceAccepted=false/wholeplanOPEN; see WARM-MATERIAL-STAGE-REASONS-20260928.md and build/warm-stage-reasons-artifact-manifest-20260928.json.

Opt-in two-slot warm material source reserve 89a9d9c6: all-five build211 and fresh actual ON541/1, OFF525/1, model-purpose/preflight68/4 PASS/noSKIP. Under existing source+jobs+preflight flags, ordinary binding admission stops at254 while stage-purpose may use the unchanged256/2MiB global limits; default flags OFF. Same installed02:02 pair DayZ020310/020622 both FAILED unchanged strict stage filename-family upload gate with owned normalexit0/no force. Both observed two usable current-bound stage sources accepted and stored; first run later had Cancelled retirement, second had two historical exact-member promotion rows. Neither had a stage Take/upload or same-source stage proof. One separate primary prepared 1398016-byte upload succeeded in each; token/key promotion does not prove eventual use. Other `_sm` binding refusals and global capacity counters are not per-source causal proof. Ordered differing work precludes causal FPS/no-regression acceptance; no default promotion/material readiness/GPU-free claim. WholeplanOPEN/defaultReady=false/performanceAccepted=false; see WARM-MATERIAL-SOURCE-RESERVE-20260928.md and build/warm-stage-reserve-artifact-manifest-20260928.json.

Opt-in nonretained SpecularDetail preflight skip 382ba018: incremental all-five build19 and focused policy32 assertions/3 cases PASS/noSKIP; clean installed02:23 matched EXE/DLL hashes (renderer Rust build ID b1fbd24c reused after C++-only change). Only an exact completed NoOwnedSection registration suppresses missing SpecularDetail header attempts; cached stage slots and NormalMap/direct DrawSectionTL remain eligible. Actual DayZ022340 had zero warm-material preflight observations, versus five road `_sm` observations in the earlier different run; one separate exact Take/source-validated1398016-byte primary upload remained. Result.passed=false solely because requested WarmPublishedReuse observed zero promotions, though owned exit0/normal shutdown/no force and returned19923/required0. Zero observed stage attempts is not all-stage proof or a strict useful-upload PASS. Ordered cache/work differs and no matched default screen exists; no causal FPS/no-regression/material/all-pass acceptance. Experimental flagsOFF/defaultReady=false/performanceAccepted=false/wholeplanOPEN; see WARM-MATERIAL-SOURCE-RESERVE-20260928.md and build/warm-nonretained-stage-artifact-manifest-20260928.json.

### Read-only Patnik warm-stage probe

Opt-in Patnik warm-stage probe 8963fa4b: all-five build263 PASS; focused TargetTrace7 assertions/1 case, ordinary StorePeek10/1 and actual warm store41/1 PASS without skips. Clean installed02:37 same-pair DayZ `patnik-probe-20260928-023957` exited0 with normal shutdown/no force and WGPU startup. At the early cut resident3/pending=true the existing bank entry was absent; at resident1487/pending=true, exact target key matched, GPU handle and upload-attempt flags were true, and initialized mips existed. Initialized/current archive binding and CanWarmPrepare remained false; the prepared store had no entry/chain/current-member match and the exact trace token was0. A key-only uploaded mark is not a prepared-worker upload. This bounded read-only snapshot proves the probe path, not natural material-stage warm work, useful Take/upload, full-route coverage, FPS or no-regression. Experimental defaults remain OFF; wholeplanOPEN/defaultReady=false/performanceAccepted=false. See PATNIK-WARM-STAGE-PROBE-20260928.md and build/patnik-probe-artifact-manifest-20260928.json.

### Actual same-frame projected camera pose

Projected actual-camera pose proof be639564: incremental all-five build247 PASS and focused projected-surface-bound7453 assertions/6 cases PASS/noSKIP; supported paired installation02:46. Root-owned `projected-pose-proof-20260928-20260928-024617-ad9e29` passed with exit0/normal shutdown/no force. Its read-only same-return frame reports requested=actual main-camera position [1200,100000,1240], camera generation60, requestId=observedRequestId6, viewport1280x720, clipNear0.0957142934, selected-union depth minimum99968.83853 and ideal upper0.0558372137. This establishes the scoped actual camera tuple and selected-union ideal interval bound in one controlled original-source fixture. Earlier horizontal100km request remained clamped and FAILED; no broader pose, source-error, pixels, visibility, all-pass, retail asset, FPS or default readiness proof follows. Experimental paging remains OFF by default; wholeplanOPEN/defaultReady=false/performanceAccepted=false. See build/projected-camera-pose-artifact-manifest-20260928.json.

### Patnik archive binding refusal, exact run

Patnik exact archive refusal cde773f5: incremental all-five build22 and pure matcher6 assertions/one case PASS/noSKIP; supported paired install03:04. Root-owned DayZ `patnik-probe-20260928-030435` exited0 with normal shutdown/no force. On the target first raw bank read, bounded opt-in reason `bindingCountCap` recorded live254/ordinary limit254 with capRefused46; FileCache miss returned bound=false and TextureBank created mips=true/initBound=false. Later FileCache hit remained unbound and existing target texture had GPU handle=true. At sampled resident1332/pending=true, archive live254/global cap256/capRefused165, exact bank key/GPU true, initialized/current binding false, no store entry and trace token0. This establishes the ordinary 254-reservation refusal for this run, not a general source-freshness theorem or per-texture cause inferred from later global counters. No cap increase, prepared stage Take/upload, default promotion or performance result. Experimental flags OFF; wholeplanOPEN/defaultReady=false/performanceAccepted=false. See PATNIK-WARM-STAGE-PROBE-20260928.md and build/patnik-source-reason-artifact-manifest-20260928.json.

### Private Auto-Fine controlled selection

Private Auto-Fine cdaaa8b8: initial all-five build248 FAILED solely at nonexistent DiskPageInput::sourceAdmissionEpoch; repaired to report.sourceAdmissionEpoch and incremental all-five build15 PASS. Focused projected-demand policy78 assertions/5 cases PASS/noSKIP. Same clean03:16 EXE/DLL pair ON `private-auto-fine-20260928-20260928-031635-ff2c70` and OFF `private-auto-fine-off-20260928-20260928-031717-0e79ce` both passed owned exit0/normal shutdown/no force. ON committed generation148/sourceRequest26 only after completed source/camera/renderer cut, selected the already prepared Fine model4 with8/8 meshes Present and no worker jobs/reservation; final retirement8/8 Absent/debt0. OFF kept private flag/commit false, prepared coarse then chose Fine manually; final8/8 Absent/debt0. This is one controlled original-source fixture and a private one-way selection, not automatic frontier/all-pass/pixel/retail/FPS/default acceptance. FlagOFF; wholeplanOPEN/defaultReady=false/performanceAccepted=false. See PRIVATE-AUTO-FINE-20260928.md and build/private-auto-fine-artifact-manifest-20260928.json.

### GPU-owned stage header intent at the global cap

GPU-owned material stage reservation d7812c1c: incremental all-five build18 PASS and existing actual reserve test fresh-env541 assertions/one case PASS/noSKIP; paired install03:21. In the bounded root-owned DayZ `patnik-probe-20260928-032226`, exit0/normal shutdown/no force, Patnik first raw bank read still refused `bindingCountCap`, now at live256/limit256/capRefused44 rather than cde local live254/limit254. FileCache miss then hit remained unbound; the target later had an existing GPU handle but no initialized/current binding, store entry or exact token. Sampled resident1487/pending and archive live256/global cap256/capRefused174. The narrow retained-GPU-stage header intent reached the full global budget before Patnik; identities of the two earlier slot consumers are unknown. No cap/threshold increase, target warm preparation, material-stage Take/upload, causal FPS or default acceptance. Experimental flagsOFF; wholeplanOPEN/defaultReady=false/performanceAccepted=false. See PATNIK-WARM-STAGE-PROBE-20260928.md and build/gpu-owned-stage-patnik-artifact-manifest-20260928.json.

### Private Auto-Coarse return and tree source probe

Private Auto-Coarse 4f147763: initial all-five build248 FAILED solely from test initialization of nonexistent PrivateAutoFineCut.withinWatchBudget; repaired incremental all-five build PASS. Focused projected-demand92 assertions/6 cases PASS/noSKIP. Same clean03:40 installed pair ON `private-auto-coarse-on-20260928-20260928-034115-e333dc` and OFF `private-auto-coarse-off-20260928-20260928-034213-cc57fe` both passed owned exit0/normal shutdown/no force. ON, after private Auto-Fine, committed fresh camera generation170/sourceRequest29 with certified ideal upper0.055837<=leave1 within3/64 observations; returned to existing Coarse model3 on the same instance3, all8 mesh handles still Present/unchanged, jobs/reservation0. Abort ended8/8 Absent/debt0. OFF left distant Fine model4 selected, Auto-Coarse false/observations0, then8/8 Absent/debt0. This is a private controlled return selection, not eviction, physical GPU release, general frontier, pixels/all-pass/retail/FPS/default acceptance. FlagOFF; wholeplanOPEN/defaultReady=false/performanceAccepted=false. See PRIVATE-AUTO-COARSE-20260928.md and build/private-auto-coarse-tree-artifact-manifest-20260928.json.

Tree fixed-source diagnostic on the same4f03:40 pair: bounded root-owned `patnik-probe-20260928-034258` exited0 with normal shutdown/no force, HotRetire OFF. Target `t_piceaabies_trunk_no.paa` first bank read was wrapped/bound=true at live122/global limit256, later GPU handle true. The first sampled cut was still resident2/pending=true with exact target key, initialized/current binding and GPU true but CanWarmPrepare=false, no prepared-store entry or exact trace token. This validates the selected tree source probe, not a naturally useful warm stage, Take/upload, wider source coverage or performance benefit. Experimental flagsOFF; wholeplanOPEN/defaultReady=false/performanceAccepted=false. See build/private-auto-coarse-tree-artifact-manifest-20260928.json.

### Cache-pressure trial and per-view reference precondition

`e45e677f` adds an opt-in, cache-reference-only pressure trim and a pure private 36-view reference-fact state machine. The former has an eight-update over-budget dwell, at most one Shape cache Ref drop per residency update, and an eight-update below-85% recovery; it never claims physical GPU reclaim. The latter has no GPU producer or renderer getter and cannot yet authorize page eviction. All-five build, focused C++54/4 and Rust7/7 tests passed; paired game build installed04:18. The single actual DayZ zero-route diagnostic `pressure-cache-diagnostic-20260928-041841` passed/clean, returned19923/pendingfalse, but its settled tracked3.229GB was below budget5.884GB and cacheSize/drops stayed0: pressure behavior was not exercised. No comparison/FPS/performance acceptance. Keep `WGR_OBJECT_STREAM_PRESSURE_CACHE_TRIM` OFF, GeometryPage flags OFF, plan OPEN/defaultReady=false/performanceAccepted=false. The next safety precondition is an actual asynchronous exact-model GPU COUNT observation for every required view, including cached/sky passes, with epoch/generation and failed-frame invalidation before any Fine retirement. See `docs/roadmap/decisions/STREAMING-CACHE-PRESSURE-VIEW-FACTS-20260928.md`.

Follow-up `0a063953` corrects the pure reference state to three bounded whole-frame slots, distinct copied/submitted view masks and a 96-byte target-identity layout; Rust9/9 PASS after one removed unsupported array-Default derive. No GPU producer or current-frame all-view certificate exists: delayed async readback can miss a newly refreshed main/cascade generation, so Fine eviction remains forbidden. Separately, a pure owned-byte raP `.rvmat` extractor with strict work limits passed focused C++40/3 and broader material578/86; two exact ignored DayZ `plants.pbo` materials (925/682 bytes) returned Complete with the observed trunk normal-map path. Neither material capture nor stage-PAA second handoff nor warm Take/upload exists. All-five build PASS, paired0a04:40 installed startup PASS with Tidewater Native. Default DayZ zero-route `rap-view-default-20260928-044224` PASS/clean returned19923/required0/pendingfalse and tracked3.229GB/5.884GB; no frame-time, route convergence or no-regression acceptance. Plan OPEN/defaultReady=false/performanceAccepted=false. See `docs/roadmap/decisions/STREAMING-CACHE-PRESSURE-VIEW-FACTS-20260928.md`.

Follow-up `ee410de8` installs the first opt-in exact DayZ tree source handoff: two physical PBO model members captured at owner Reset, then two exact raP `.rvmat` material members captured at owner SubmitConvert and parsed on the worker after source/generation checks. All-five desktop build and focused9/1 fixture assertions PASS. Paired game startup PASS with default Tidewater Native. One short actual DayZ tree route `patnik-probe-20260928-050812` PASS/clean/no force, with two exact identity-leased model requests, tree `mode=async`, and worker raP status0 for both physical materials (925/682 bytes), yielding the expected trunk normal-stage path. This is historical generation-scoped source discovery, not current binding, second-stage PAA capture, prepared StoreTake/upload, full route or performance proof. Both new flags remain OFF; whole plan OPEN/defaultReady=false/performanceAccepted=false. See the same decision record.

Follow-up `5c4bd185` moves a bounded names-only raP candidate through `TakeConverted` to the visual owner. All-five build and raP23/2 PASS; paired installed05:25 DayZ `patnik-probe-20260928-052531` PASS/clean/no force. Both exact sources produced generation-1 owner candidate rows for the expected trunk normal stage, with default Tidewater Native selected. No current source binding, PAA capture, prepared StoreTake/upload, comparative FPS or no-regression proof follows; flags OFF, plan OPEN/defaultReady=false/performanceAccepted=false. The next material task is owner-side current-stage binding validation before any warm submission. Independently, spare GPU cull counter word +20 is a possible default-off target-model COUNT diagnostic; real per-view readback, generation coherence and failure invalidation remain unbuilt, so Fine eviction stays forbidden. See `docs/roadmap/decisions/STREAMING-CACHE-PRESSURE-VIEW-FACTS-20260928.md`.

Follow-up `7f4efbad` adds a third default-off exact-fixture switch for owner capture of a current initialized normal-map PAA binding and one bounded warm submission. All-five build and fresh warm-enabled151/7 PASS; paired installed05:37 DayZ `patnik-probe-20260928-053725` PASSclean/no force with default Tidewater Native. The first exact model logged current `captured=true submit=0`; the second found the shared texture already used and fell back. The sampled store had no entry/chain or exact warm trace token while the normal map was GPU resident with upload attempted. This proves admission only, not worker Put/Take, useful upload, matched latency/FPS or no-regression. Immediate ordinary registration can outrun the worker; flags OFF and whole plan OPEN/defaultReady=false/performanceAccepted=false. See the same decision record.

Follow-up `525a55ca` adds a default-off exact-model GPU survivor COUNT in spare per-view cull stats word +20 and decodes it in the existing aggregate diagnostic. Actual headless GPU test printed 8 target of 9 visible instances, with reflection zero when undispatched; all-five desktop build and paired installed05:49 WGPU startup PASS with Tidewater Native default. The readback is not yet bound to all required pass submissions, source/target epoch or cached sky/local view generations, so no absence certificate, Fine retirement or GPU reclamation is permitted. No performance/no-regression acceptance; plan OPEN/defaultReady=false/performanceAccepted=false. See `docs/roadmap/decisions/STREAMING-CACHE-PRESSURE-VIEW-FACTS-20260928.md`.

Follow-up `ea4199b7` adds an unused private, lazy three-slot main-view COUNT readback. Actual-GPU tagged 8/0 samples passed after correcting two test arm-order failures; all-five desktop build, paired installed06:05 startup and Tidewater Native default passed. No production owner caller, real source generation, draw/submit closure witness, or non-main/cached view coverage exists. A successful render return may mean swapchain acquire was skipped, so it cannot be treated as submitted COUNT. Keep Fine retirement and physical reclamation forbidden, experimental flags off, and whole plan OPEN/defaultReady=false/performanceAccepted=false. See the same decision record.

Follow-up `c467fc9b` registers the exact raP tree-stage physical member in the bounded warm provenance trace. All-five desktop build and focused60/6 PASS; paired installed06:15. The single short DayZ tree smoke `patnik-probe-20260928-061555` exited cleanly and showed untruncated token1 Accepted→Submitted→WorkerStarted→StorePut→WorkerPut→OwnerExactAttempt→StoreTakeWarm→SourceValidated→UploadSucceeded (2,796,032 bytes), followed by a normal-map binding. This proves one useful current-source stage upload. A proposed nonblocking installation hold is unnecessary for this fixture and was not implemented. No matched FPS/no-regression, wider coverage, route convergence, all-pass/visual or default acceptance; narrow flags OFF and whole plan OPEN/defaultReady=false/performanceAccepted=false. See the same decision record.

Follow-up `d6801517` binds the opt-in main GPU COUNT diagnostic to a verified original-source GeometryPage fixture and to actual final-encoder submission. Versioned FFI and a nonblocking getter keep skipped-acquire/error/stale-identity results Unknown; default rendering has no staging allocation or extra poll. Focused FFI and actual-GPU 8/0 tests, all-five build and paired installed06:34 WGPU startup PASS. The first fixture run passed but its runner had cleared the inherited probe flag; explicit `-MainCountProbe` then produced a clean passing original-page run `main-count-active-20260928-063717-d470f2` with submitted coarse/fine owner requests and matching later main Present count1 facts. Fixture water was explicitly Current OP; ordinary installed startup still selected Tidewater Native. No draw/pixel/all-view absence, physical reclamation, matched FPS/no-regression or default acceptance; Fine retirement forbidden and whole plan OPEN/defaultReady=false/performanceAccepted=false. See the same decision record.

Follow-up `f7ed6840` makes the private main COUNT copy conditional on recorded GPU-driven main colour commands and a closed colour pass, as well as actual final-encoder submission. All-five build and focused actual-GPU Present8/Absent0 test passed; paired installed06:50 WGPU startup passed with Tidewater Native default. The single active original-source fixture `main-draw-closure-20260928-065127-a82ae2` passed/normal and produced submitted requests plus two later current Present1 facts. It explicitly used Current OP water. The witness proves encoded main draw closure, not pixels or all-view absence; no real-game zero, Fine release/reclaim, matched FPS/no-regression or default acceptance. Probe OFF and plan OPEN/defaultReady=false/performanceAccepted=false. See the same decision record.

Independent Tidewater W7k source `c31600e0` is now fully merged through `c0ca8413`; all-five build and 47 focused Tidewater tests passed, and installed startup selected Tidewater Native. The showcase runs beyond its old early-stop point. Two successive ending defects were diagnosed (unsupported `endMission`, then a camera-held transition); `a6defd61` uses the supported command after releasing the camera, and an installed short end-tail fixture exited 0 normally. Full visual inspection and integrated matched performance remain pending. The water work does not change GeometryPage policy or the streamer's OPEN/defaultReady=false/performanceAccepted=false status. See `docs/roadmap/decisions/TW-WATER-W7K-INTEGRATION-20260928.md`.

Follow-up `eeb7c04d` fixes an opt-in COUNT precondition for standalone reflection/sky/GI culls: their uploaded params now inherit the current target-model selection instead of leaving the diagnostic disabled. One actual-GPU sky test produced count1 for a visible selected model and count0 after selecting an absent model in a freshly cleared dispatched view. All-five build and paired installed07:26 WGPU startup passed with Tidewater Native. There is still no real per-view readback/publication generation or cached-view proof, so Fine retirement/reclamation, matched FPS/no-regression and GeometryPage default acceptance remain forbidden. See `docs/roadmap/decisions/STREAMING-CACHE-PRESSURE-VIEW-FACTS-20260928.md`.

Follow-up `e6d6f5f5` keeps an empty required-view mask inconclusive in the unused GPU view-reference state machine. Nine focused tests and all-five build passed; paired installed07:35 WGPU startup passed with Tidewater Native. Follow-up `46d66700` requires an actual cascade cull dispatch, encoded indirect depth draw and closed depth pass before fixture shadow facts are recorded. Six fact tests, one actual-GPU cascade test, all-five build and paired installed07:40 WGPU startup passed. One original-source fixture `cascade-pass-witness-20260928-074115-91fcd8` PASSclean/normal/no force and reported cascadeDrawMask15 at coarse/fine/returned fallback. These are recording witnesses, not target-model COUNT, final-submit/pixel/all-view absence, Fine release/reclaim, matched FPS/no-regression or default readiness. Flags OFF; whole plan OPEN/defaultReady=false/performanceAccepted=false. See `docs/roadmap/decisions/STREAMING-CACHE-PRESSURE-VIEW-FACTS-20260928.md`.

Follow-up `98923801` integrates the separate default-off cascade-0 target-model COUNT readback (solar cull + closed indirect depth pass + final submit), rejects late submits in unused view-fact bookkeeping, and carries up to two exact raP stage names per selected tree material within the prior 2 KiB cap. Ten view-fact Rust tests, actual-GPU main/cascade8/0, FFI layout, raP26assert/2cases and all-five desktop build PASS. Paired installed08:08 startup PASS/Tidewater Native default; exact original-source fixture cascade0-count-integrated-20260928-080859-766c49 PASSclean/normal with current main+cascade Present1 on tokens3/6/7. Native Reforger streamer route native-streamer-20260928-20260928-080932 PASSclean/normal:19999 resident,0 pending before and after eight fixed poses, tracked GPU1695899168→1707629924B. DayZ exact tree capture rap-handoff-capture-20260928/integrated PASS/normal and emitted second trunk_dxt5.paa owner candidate. These are selected positive diagnostic and source-name observations, not all-view zero, ready texture binding, matched FPS/no-regression, Fine release/reclaim, or default GeometryPage acceptance. Reforger rendering anomalies are not correctness ground truth. Flags OFF; whole plan OPEN/defaultReady=false/performanceAccepted=false. See STREAMING-CACHE-PRESSURE-VIEW-FACTS-20260928.md and STREAMING-CASCADE0-TARGET-COUNT-20260928.md.

An installed same-build/same-profile one-pair `perf_water.eden` screen at1600×900/native/4×MSAA completed normally and uncontended for both explicit water backends on `98923801`. The fixed-view legacy capture measured GPU23.560ms/CPU average25.573ms and Tidewater measured GPU14.848ms/CPU average16.202ms; the screenshots retain the same view but water appearance differs. This is a single-view water screen only, not route-wide matched streamer performance or Tidewater visual acceptance; streamer performanceAccepted=false and plan OPEN. See `docs/roadmap/decisions/TW-WATER-W7K-INTEGRATION-20260928.md` and `build/stream-residency/water-pair-20260928`.

Follow-up `4bc94c9c` combines four separate opt-in solar COUNT rings and an opt-in GI sun-proxy view5 ring with bounded current-source capture of a second exact DayZ tree stage. Focused actual-GPU main+4cascade+GI Present8/Absent0/Unknown, FFI and raP37assert/3cases plus all-five desktop build PASS; paired installed08:51 startup PASS/Tidewater Native. Installed original-source fixture sun-gi-count-integrated-20260928-085155-25a4e3 PASS/normal and owner-validated main+solar0-3 Present1 tokens3/7. GI has no installed owner-logged fact yet. One DayZ opt-in short run rap-second-stage-20260928/integrated PASS/normal: current trunk_dxt5.paa source submitted, worker Put, but no later Take/upload; normal-map source again reached source-validated2796032B upload. Experimental flags OFF; no all-view zero, physical Fine release, matched streamer FPS/no-regression or default acceptance. Plan OPEN/defaultReady=false/performanceAccepted=false. The separately requested Reforger material audit found a native roughness/metalness translation gap, documented in `docs/roadmap/decisions/REFORGER-MATERIAL-PBR-AUDIT-20260928.md`; no native shader edit or visual acceptance was made.

The second tree stage no-Take is explained by missing WGPU demand: Stage 2 is `MacroAmbient`, and the current WGPU tree material path has no MacroAmbient consumer. `c9f1d21d` reverted only the added second-stage warm job; the earlier names-only raP discovery and useful normal-map warm path remain. Historical `rap-second-stage-20260928` observations describe the reverted opt-in experiment. `0757dabc` separately adds owner-validated GI COUNT logging to the original-source fixture, pending its first integrated runtime check. Experimental flags remain OFF and plan OPEN/defaultReady=false/performanceAccepted=false.

Follow-up `de82e553` integrates the separate opt-in local shadow tile0 COUNT ring and owner logging for it and GI. A fresh tile has its own day/night cull routing; cached tiles and aborted submits stay Unknown. Headless GPU positive day/night and zero/abort tests, all-five build, paired installed startup and one original-source fixture passed. The installed fixture observed current main plus solar0-3 Present1 on tokens5/6, but no GI owner fact and no local light, so those two views remain runtime-unproven. This is still no all-view absence, Fine reclaim, matched FPS/no-regression or default GeometryPage acceptance. `de82e553` also includes an explicitly opt-in Reforger EMAT roughness/metalness source-provenance record, without shader change or visual improvement; see `REFORGER-MATERIAL-PBR-AUDIT-20260928.md`. Whole plan OPEN/defaultReady=false/performanceAccepted=false.

Follow-up `b0af9c75` adds default-off exact planar-reflection COUNT with its separate early queue submission and later final-frame commit; disabled/skipped/aborted reflection remains Unknown. Focused actual-GPU Present8/Absent0/skip/abort and FFI tests passed, all-five build passed and paired WGPU startup passed. The initial install stamped `+dirty` due to uncommitted documentation despite clean committed code; restamp and installed reflection fixture remain pending at this ledger checkpoint. This is still diagnostic only, not all-view zero, Fine release/reclaim, matched streamer performance or default readiness. Plan OPEN/defaultReady=false/performanceAccepted=false.

After `4cacc947` committed the documentation, the same paired build received a clean installed stamp and passed WGPU startup. The single exact original-source fixture `reflection-count-integrated-20260928-095037-d22765` passed/normal and owner-logged planar reflection Absent0 against simultaneous main+solar0-3 Present1 for tokens1/3/4. The distinct per-view result is useful evidence of reflection routing, not an all-view absence certificate; GI, local and cached views still block Fine eviction. No matched streamer FPS/no-regression or default acceptance; plan OPEN/defaultReady=false/performanceAccepted=false.

Follow-up `ec9eacbf` / `7084be0a` supplies an opt-in zenith sky0 COUNT ring and owner getter. Focused real-GPU Present8/Absent0/cache/abort, FFI and all-five build passed; paired installed startup passed. Exact original-source `sky0-bcr-count-integrated-20260928-100923-5217ef` passed/normal with owner sky0 Present1 tokens3/7 and GI Present1 token8. Native Reforger eight-pose `native-bcr-sky0-20260928-20260928-101030` passed/normal and returned to 19,999 resident/0 pending, tracked GPU 1,692,753,536 to 1,707,629,924 bytes. These do not establish matched FPS/no-regression or native visual correctness.

Follow-up `9ab82ed3` adds a default-off CPU zenith-cache publication generation requiring the actual cull, closed draw, final submit and successful FFI return; abort retries. Focused Rust tests and an integrated all-five build passed. It does not create a default GPU readback or establish GPU completion, all cached view coverage, all-view absence or Fine retirement. Reforger `_BCR.edds` roughness-alpha preservation (`8ec069f4`) and the legacy `_BCR.dds` guard (`4a85b88a`) likewise have passed the combined build, but no native PBR shader visual acceptance. GeometryPage paging/reclaim remains OFF and the plan OPEN/defaultReady=false/performanceAccepted=false.

Follow-up `64129fe6` adds a second default-off CPU publication generation for local shadow tile0, with exact day/night cull routing, closed GPU depth draw, final submit and successful render return. Six focused Rust tests passed; integrated build/installation are pending at this checkpoint. This is not GPU-completion, cached-view COUNT, all-view absence, Fine reclaim or default acceptance. Continue with per-view cache-generation coverage and only then revisit eviction authority and matched performance.

The `d0777019` combined all-five build and paired installed WGPU startup passed. An opt-in native Reforger eight-pose route `native-cache-publication-20260928-20260928-103212` passed/normal, returning to 19,999 resident/0 pending and 1,707,629,924 tracked GPU bytes. The route did not capture the CPU publication rows, so it is a stability check, not direct publication, GPU completion or no-regression evidence. Experimental flags remain off by default; plan OPEN/defaultReady=false/performanceAccepted=false.

Follow-up `161737a4` generalizes the opt-in CPU submit/return publication generation to all five interior-sky directions, with exact view/target/epoch identity and abort retry. Seven focused tests passed; combined build/installation are pending at this checkpoint. The 36-view absence state machine is still unused; only selected views have exact COUNT rings and cached local tiles 1–23 plus GI still lack post-submit generations. An eventual batched 144-byte count readback is plausible, but cannot substitute for those witnesses. Fine reclamation and GeometryPage default promotion remain forbidden.

Follow-up `1ef64503` supplies a default-off GI RSM CPU publication generation with real view5 cull/closed draw, matching view/epoch, final submit and successful FFI return. It leaves the ordinary 8-frame budget and clear-only cache transition intact. In opt-in mode shader sample availability is separate from the stricter cache proof, avoiding a periodic missing-bounce frame. Five focused tests passed; integrated build/installation are pending. Local tiles 1–23, cached COUNT records, GPU completion and all-view absence remain open; Fine reclamation and default promotion stay forbidden.

Follow-up `b8b73be4` / `eb51ae31` extends the default-off CPU publication generation to all 24 local shadow tiles, with tile0-only compatibility flag preserved. Six focused tests passed; combined build/installed check remain pending. It adds no default GPU work, but still lacks COUNT results for local tiles 1–23 and the private 36-view state machine is not wired. The current original-source COUNT request also carries a synthetic producer-model-derived generation rather than an actual model-content revision, so even a batched readback cannot authorize Fine release. Next production source work must bind a monotonic model registration/content revision and invalidate on dependent mesh/material changes. Whole plan OPEN/defaultReady=false/performanceAccepted=false.

The combined all-five build and paired installed startup for the five interior/GI/24-local CPU publication hooks passed on `223114f4`; they still give CPU submission witnesses only, with no new default GPU work. The installed W10g `perf_water.eden` screen exited normally at 1600x900/native/4x MSAA with Tidewater Native, GPU frame 14.946 ms and CPU frame average 16.463 ms. This is one pose, not matched streamer performance or owner visual acceptance; see `TW-WATER-W10G-INTEGRATION-20260928.md`. Independently, the two missing `dec/tidewater-w11b-boat-planing` commits were ported as `71600cda` and `b993b447`. The combined desktop build and focused ship tests (10 assertions) passed; installed boat behavior remains unverified at this checkpoint. W11a/W11b do not change the GeometryPage gate: OPEN/defaultReady=false/performanceAccepted=false, no Fine release or physical reclaim. See `TW-WATER-W11B-INTEGRATION-20260928.md`.

Follow-up `a3a41cdf` replaces the original-source fixture's synthetic COUNT generation with an opt-in drain-owned successful-registration revision plus renderer incarnation and producer/Rust model identity. Duplicate live model/mesh handles, relevant mesh destruction, retirement and handle exhaustion fail closed; the main request and six asynchronous view-publication sites recheck freshness. The normal GeometryPage policy and default GPU work are unchanged. This closes the synthetic-label defect only for the diagnostic path; source-level checks passed, while integrated compile/runtime verification and dependent mutation tests remain pending at this checkpoint. All-view COUNT/GPU completion and Fine release are still open; defaultReady=false/performanceAccepted=false.

Follow-up `ea8248e1` extracts the same opt-in revision state into a small production header and adds Catch2 lifetime tests for retire/re-register, duplicate poisoning, mesh death, missing dependencies, incarnation mismatch and handle/revision overflow. A standalone clang++ transition run passed; integrated tests/build are pending at this checkpoint. The two missing W11c/W11d Tidewater commits were also ported as `649fa616`/`7dc107e6`, with the W11d diagnostic accumulation guarded off in ordinary play. W11c's 9x/12x planing gain supersedes W11b's initial 20x/26x. Build and installed boat/streamer runtime checks are pending; see `TW-WATER-W11D-INTEGRATION-20260928.md`. No Fine release or default GeometryPage promotion follows from these patches.

The combined game/test build for `ea8248e1` plus W11c/W11d passed, and the focused ship/source-revision Catch2 run passed 45 assertions in seven cases. The latter exercises identity and lifetime transitions in the same opt-in revision state used by `EngineWgpu`; it does not itself prove integrated original-source GPU COUNT publication. Installed fixture and boat visual checks are still pending. GeometryPage remains default-off, with no Fine release or physical reclamation authority.

The paired `c7e1a12c` game build is installed; WGPU `--check` passed with Tidewater Native. A bounded PBR chase screenshot run passed/normal at 1600x900, measuring GPU frame 12.049 ms and CPU frame average 12.788 ms at that single moving-camera pose; no matched performance or jitter acceptance follows. One installed original-source fixture `source-revision-w11d-integrated-20260928-114838-19c53d` passed its geometry contract and normal exit; main COUNT requests were submitted and planar reflection Absent0 published with live source revisions 4–6. It logged no main/solar Present rows, unlike an earlier baseline. The fixture's rapid request/model transitions and approximately 200 ms screenshot frames plausibly closed the asynchronous publication window; a narrow test-harness hold is being prepared. Treat main/solar publication as unconfirmed on this build, not as a zero-count or absence certificate. Experimental flags remain off; plan OPEN/defaultReady=false/performanceAccepted=false, no Fine release.

The `80580d1f` test-runner follow-up adds a `-MainCountProbe`-only, bounded three-second hold at stable coarse, first fine and refilled fine identities. It waits for exact main plus solar0–3 Present1 rows before advancing the fixture, so a slow screenshot cannot silently turn a valid asynchronous result into an inconclusive run. PowerShell parsing passed; installed rerun is pending while Oliver uses the shared game. Ordinary fixture timing and runtime code are unchanged.

The held COUNT fixture was run three times after the game became free. One run logged exact main and solar0–3 Present1 for one request, but failed a later request; two cleanly exited runs did not yield the required rows even after bounded extra screenshot captures. This is positive evidence that publication can occur, not a reliable fixture or all-view acceptance. `aeba48b3` adds an opt-in, three-slot 108-byte COUNT batch for interior directions 1–4 and local shadow tiles 1–23. It requires each view's fresh cull, closed indirect draw, final submit, successful render, mapping and matching request/epoch; cached or missing evidence stays Unknown. The default path has no added GPU copy or allocation. The combined desktop build, 54 Tidewater Rust tests, targeted actual-GPU COUNT and FFI layout tests, and 45 ship/source-revision C++ assertions passed. Paired installed startup and native Reforger stability verification remain pending. Historical cached-view generations, GPU completion and 36-view closure are still missing; Fine release and GeometryPage default promotion remain prohibited.

The paired `22ba2e4e` build passed installed WGPU startup and one settled native Reforger Village route. The route registered 20,683 objects and completed normally at 33.741 ms GPU frame time; this one pose is not a matched no-regression measurement or a shader-material acceptance. The original-source fixture race was traced to `pollCoarse`/`pollFine` request IDs being superseded before asynchronous COUNT mapping. The test runner now waits for exact positive rows while each request is still current, without weakening the production freshness gate; this runner change is awaiting its installed check. Follow-up `13e436f9` adds a bounded, opt-in main-queue completion callback for the exact main COUNT diagnostic request. Three focused witness tests and the combined desktop build passed. Completion of preceding queue work is a different fact from visible pixels, other views or physical memory release; cached-view generations, all-view completion and Fine authority remain open.

The paired `dd5a8ba7` build passed installed WGPU startup. One bounded `-MainCountProbe` original-source run (`source-count-current-request-20260928-20260928-124404-6053c1`) passed its full geometry contract and exited normally. The log contains matching main plus solar0–3 Present1 and exact main-queue completion for coarse request 3 (token 1, source revision 4), Fine request 7 (token 3, revision 5), and refilled Fine request 13 (token 7, revision 6). This closes the observed harness request-race and demonstrates the narrow queue completion path for those three live generations. It does not bind cached historical views to COUNT, prove all 36 views absent, measure a matched FPS change, or authorize Fine release/reclaim/default GeometryPage use.

Follow-up `a308f529` adds default-off sky0 cached COUNT binding to the exact CPU publication generation, sky image allocation generation, bit-exact view, instance epoch and target source. A mapped Present/Absent fact retains its original request token on matching cache reuse; refresh, abort, resize, view/source/epoch changes and late old maps fail closed. The combined desktop game build, two state tests and focused actual-GPU sky0 test passed. Installed startup and a live cached-view demonstration remain pending. The same binding is being extended to the other cached interior, GI and local shadow views; Fine release remains prohibited.

The next opt-in slices `7f1a8092`/`fe1bfa26`, `0acbca27` and `fae4bd21` extend that exact published-image binding to interior directions 1–4, GI RSM view 5 and local shadow tile 0. The interior batch binds only copied words from a successful same-request publication; GI includes its own RSM allocation generation and rejects drawless refresh; local0 includes atlas generation/shape, light key and day/night cull index. Refresh, failed/aborted submission, source/epoch change and late old mapping remain Unknown. These paths are default-off and still do not authorize Fine release. The integrated Rust library check and focused state/publication/actual-GPU tests passed (16 focused tests); source `b717811f` passed the combined desktop build and paired installed WGPU startup. Local tiles 1–23 still have raw COUNT but no historical-cache binding; all-view completion and a 36-view closure certificate remain open.

Tidewater W12d was separately integrated as `e2396319`/`8b7148db` after checking that its older boat-drag patch was already represented. The paired installed build passed WGPU startup. The owner can now judge the mesh-exact waterline in the showcase; visual and matched performance acceptance remain open. This water work does not change the default-off GeometryPage policy or authorize Fine release. See `TW-WATER-W12D-INTEGRATION-20260928.md`.

Follow-up `2e8feec7` adds historical COUNT binding for local shadow tiles 1–23, including exact copied-word and published-image gates, atlas allocation/shape and day/night cull route. Focused state and strict actual-GPU tests passed; four failures in the broader Rust library suite have not been checked against the base and are not claimed as regressions or baseline failures. The integrated build and installed check await the owner's current game session. The 36-view atomic snapshot, applicability proof, pixels, physical Fine release/reclaim and matched performance remain open; GeometryPage stays off by default.

Follow-up `f425a651` adds one private 36-view snapshot getter over the existing COUNT producers. It demands the exact current request's committed final-queue completion, exact target/source/epoch and published image bindings for historical cache facts. It requires all 36 bits and leaves inactive/skipped views Unknown, so a complete subset cannot be mistaken for all-view absence. Twelve state tests and one FFI layout test passed; integrated build/installed verification is pending. A frozen authoritative per-request applicability mask and same-epoch cache coverage remain open, as do pixels, Fine release/reclaim and matched performance. The getter has no default GPU work or policy consumer.

Follow-up `472756be` now freezes the applicable subset of the 36 views for each exact request before resource and draw gates. Configured solar/local counts, interior intent, current GI RSM eligibility and planar policy decide the mask; an enabled view skipped later remains Unknown. Fourteen focused Rust tests, the combined `PoseidonGame`/`PoseidonTests` build and paired installed WGPU startup passed on `dea029a6`. The snapshot is still a private diagnostic with no production Fine-release consumer; cached generations may not cover the current epoch, and queue completion is not pixel visibility or physical memory reclamation. GeometryPage remains off by default and matched no-regression performance is open. The independent optional DayZ NormalMap-only warm handoff (`db7c2a08`) also builds and passes 33 focused raP assertions, but its installed DayZ ON/OFF route and performance are open. See `STREAMING-CACHED-VIEW-COUNT-20260928.md`.

Follow-up `59345e40` makes the private snapshot's pending-copy count truthful for committed current-request COUNT readbacks and marks unavailable drop telemetry explicitly. Fifteen focused Rust tests, the paired desktop build and an installed WGPU `--check` passed after a first attempt encountered a closed output pipe. This has no normal-path GPU work, Fine-release authority or measured FPS benefit; the all-view/pixel/physical-reclaim gates remain open. See `STREAMING-CACHED-VIEW-COUNT-20260928.md`.

The current global retained-instance epoch can change every frame in a moving scene, while interior sky refreshes one direction per frame and GI RSM refreshes on a longer budget. Requiring all cached images to bear the same global epoch correctly fails closed, so relaxing it without narrower mutation evidence could publish false absence. `b02ee192` starts that evidence with an optional retained-static target-model revision; A/B mutation, handle reuse, retirement and overflow tests, the combined build and installed WGPU startup passed. It is not yet consumed by cached COUNT. Dynamic instance tails and section/mesh/material/pose changes remain untracked, and the existing global gate stays intact. No all-view closure, Fine release or default GeometryPage promotion follows. See `STREAMING-CACHED-VIEW-COUNT-20260928.md`.

Follow-up `f80ad349` makes the optional target revision include exact ordered dynamic-instance rows when armed, with no normal-path scan/allocation. A new transition test, Rust compile, the combined build and installed WGPU startup passed. This exposes an existing diagnostic limitation: `set_dynamic` did not change the global instance epoch, so historical cached COUNT facts for a dynamic target cannot rely on that epoch alone. The target revision has no cache consumer yet; future cached facts must stamp and recheck it, and unknown mesh/section/material/pose edits must still fail closed. COUNT itself reports cull survivors, not pixels. No Fine release or default GeometryPage promotion follows.

Follow-up `bc292e66` advances the optional revision on actual resolved-section changes and successful on-renderer mesh vertex writes. Focused tests, Rust compile, paired build and installed WGPU startup passed. It leaves ordinary GPU work and visual caching unchanged. The separate off-handle uploader plus texture and palette mutations still lack a revision witness; therefore no cached COUNT relaxation, Fine release or default promotion follows.

Follow-up `458c54bf` observes successful off-handle mesh writes through an opt-in serial inside the uploader's existing CPU lock. The private 36-view getter samples before and after, advancing the armed target witness on a change, without a GPU wait or normal-path serial allocation. Focused witness and uploader GPU tests, Rust compile, paired build and installed WGPU startup passed. The COUNT result does not yet bind or invalidate against that witness; texture and palette changes also remain open. Do not promote cached absence, Fine release or GeometryPage defaults from this diagnostic groundwork.

Follow-up `f3639c2c` now stamps the opt-in target revision at COUNT request arm and rechecks it at copy, async harvest, raw/cached getters and the aggregate FFI output boundary. Dynamic target mutation or a late off-handle upload yields Unknown rather than a stale Absent. Focused Rust tests, the paired desktop build, installed WGPU startup and one complete W12e Tidewater showcase passed. Global epoch and cache-image gates remain unchanged, so moving scenes can still leave the 36-view snapshot incomplete. COUNT zero is a sphere-cull fact, not visible pixels or physical memory reclamation. Fine release and GeometryPage default promotion remain forbidden; matched performance is open. See `STREAMING-CACHED-VIEW-COUNT-20260928.md`.

Follow-up `a8194313` adds an exact cull-input key to opt-in cached interior/GI/local COUNT publication and lookup. This prevents reusing an old count under changed frustum, camera, distance or subpixel settings even when the image key itself still matches. Focused Rust checks, paired desktop build and installed WGPU `--check` passed at `7b1089b5`. Global epoch and image/source gates remain unchanged, with no Fine release/default promotion or matched performance claim.

Follow-up `60b36d06` adds an internal read-only diagnosis that separates exact target COUNT input equivalence from image publication epoch for cached local, interior and GI views. Thirteen focused Rust tests, paired desktop build and installed WGPU `--check` passed at `adb19215`. The existing cache, absence and Fine gates are unchanged. An epoch match alone does not witness foreign dynamic casters, meshes, materials, alpha textures or poses; no image-current or performance acceptance follows. See `STREAMING-CACHED-VIEW-COUNT-20260928.md`.

Follow-up `6d4226eb` adds an opt-in source-local witness for bit-identical retained-static instance updates across global epoch changes. Focused Rust tests, `cargo check`, paired build and installed WGPU `--check` passed at `b24b117a`. Dynamic casters and mesh/material/texture/pose changes remain outside this witness; cache/image/Fine gates and default GeometryPage policy are unchanged. See `STREAMING-STATIC-NOOP-EPOCH-20260928.md`.

Follow-up `4d975cce` adds an opt-in ordered dynamic-instance row mutation serial, including foreign casters that leave the selected target COUNT and global retained-instance epoch unchanged. Three focused Rust tests, the paired desktop build and installed WGPU `--check` passed. It is diagnostic-only and does not prove a cached image current or change cache/Fine gates; mesh/material/texture/pose and CPU casters remain separate. See `STREAMING-DYNAMIC-IMAGE-MUTATION-20260928.md`.

### 29 September 22:47 — independent hierarchy and physical parent uploads

`823270ae`/`232e2da7` passed the combined game/test build, 3,921 hierarchical
assertions and 376 mapped-PBO material assertions without skips; the paired
installed WGPU startup passed. The installed original RAM hierarchy exercised
authored fallback, root64/Fine512/root reuse, two independently staged pages
and all-eight-record retirement, then normal shutdown. Its harness verdict
failed solely on an overly narrow final one-shot rearm expectation; the actual
Invalid/HierarchyAdmissionUnavailable refusal was safe and is now accepted
explicitly, with the failure artifact preserved. The DayZ physical-parent
route passed/normal and proved 38 source/mount/layout-matched uploads. Its
230.8 ms creation is an ordered observation, not matched no-regression/FPS
evidence. The installed COUNT diagnosis failed publication normally and
identified palette mutation kind6 at revision1→2/epoch4 unchanged. Next work
adds source-bound local cuts, independent authenticated disk pages and bounded
proxy preparation; actual source writes still invalidate COUNT. Whole plan
OPEN/defaultReady=false/performanceAccepted=false; no Fine reclaim or Nanite
equivalence claim. See the three 20260929 decision records.

### 29 September — independently addressed disk pages and bounded local demand

The next combined source adds the optional authenticated `hierarchy.ghp`
producer, metadata-only disk admission, nonblocking one-page worker handoff,
source-bound intermediate/local cuts and a three-cut retained cache. Fresh
producer authority and per-page digests precede publication; authored/root
fallback remains complete during partial residency. A pure forty-view policy
supports actual perspective and orthographic matrices without pretending the
baked CLOD error is a certified pixel error. After documented build/test
fixture repairs, game/tests/tools build and 29,545 focused assertions/33 cases
passed. Pure release/refill preflight passed83/4 and DayZ physical/proxy checks
passed455/4 without skips. The opt-in palette repair separates rigid target
COUNT from foreign cached-image mutations; twelve Rust tests passed. Installed
combined validation is pending. Automatic retail paging, all-view collection,
release/refill consumer, physical byte convergence and matched performance
remain OPEN/defaultReady=false/performanceAccepted=false.

### 29 September late installed validation and next lifetime step

Committed `a525fef8` passed paired installed startup. The repaired independent
disk/local runner passed/normal: authored2/root64/local256/Fine512/root reuse,
three independent reads, fourth-cut/stale-token refusal and eleven-record
Abort absence. The first run's early page-residency stopping race remains
documented. The DayZ physical/proxy route passed/normal and consumed five
same-source proxy textures (3,320,288 bytes); speculative coverage remained
incomplete. Native Reforger's normal eight-pose route passed/normal, returning
to19,999 resident/zero pending and1,707,629,924 tracked GPU bytes. Reforger
compatibility anomalies are not rendering ground truth; these runs do not
establish matched FPS/no-regression. The COUNT run logged current main and
four solar positives for coarse/Fine, but failed a returned-request harness
race; repaired verdict pending. The next unbuilt source adds explicit root
preserving nonroot record retirement and fresh-ID refill. Agents are preparing
an actual completed-frame view collector and bounded resumable DayZ owner
registration accounting. Experimental geometry remains OFF; automatic
retail paging, physical byte convergence, comparable-quality performance and
Nanite equivalence remain OPEN/defaultReady=false/performanceAccepted=false.

The repaired exact installed `a525fef8` COUNT run
`count-palette-returned-sep30-20260929-235938-a6e2bc` subsequently passed,
normalShutdown=true/no force: returned coarse/Fine/refill models3/4/5 each
produced current main plus solar0–3 Present1, with reflection Absent0 on the
last refill token9. This closes the runner race and narrow positive-path
verification only; cached/local/interior/GI all-view absence remains open.

### 30 September — returned-root release, actual views and bounded preparation

The next source integrates explicit nonroot retirement/fresh refill, completed
actual CPU view collection (36 IDs/40 rows, private8096-byte additive ABI),
and a pure RH combined-light projection policy. Required Unknown views remain
in the frozen set; C++ joins the actual main packet before exporting summaries.
No default GPU copies/readbacks or automatic paging consumer are added.
An unused resumable owner-admission precursor enforces source/lease checks,
one upload/mesh per update, complete model acknowledgement and retained
cancellation debt. Generated-role handling and a no-upload final material
resolver remain prerequisites before DayZ runtime integration. After documented
test fixture/literal repairs, the combined game/tests/tools build passed,
281 assertions/16 C++ cases and6 Rust snapshot cases passed. Exact committed
installed release/refill/tuple validation remains pending. Automatic retail
paging, source-bound outside-view applicability, physical convergence and
matched performance remain OPEN/defaultReady=false/performanceAccepted=false.

### 30 September — installed refill and actual-view closure

Exact committed `7d3bc635` passed paired installed startup and
`hierarchy-refill-views-sep30-20260930-002012-78283f` passed/normal, without
forced termination. Root64/model3 survived detail retirement; six old handles
became Absent, one independent-page refill created Fine512/model5 with six
fresh handles, root reuse read nothing further and Abort observed all fourteen
historical handles Absent. Worker debt settled to zero. The same run joined
all twelve enabled completed-frame camera rows (main, four solar, five interior,
GI, reflection); no local lights were enabled. This verifies private record
lifetime and camera provenance, not physical memory convergence or performance.
Next source connects these actual tuples to explicit projected cut selection,
with immutable-source vertex gates and conservative whole-object outside
checks. Geometry remains default-off; whole plan OPEN/defaultReady=false/
performanceAccepted=false, automatic retail paging and Nanite equivalence open.

### 30 September — projected far/near/far and next automatic consumer

Exact `1beeea3c` installed WGPU startup and the original-source actual-view
far/near/far route passed/normal. Completed generations61/80/108 selected
root64/Fine512/root64, reused root model3 and performed1/2/2 independent reads.
Near clipping forced Fine; outside-view filtering remained source-bound.
Next source implements a <=10Hz private binary root/Fine owner consumer with
three known lower-allowance observations before coarsening and frozen pending
transactions. It retains pages/cuts and introduces no automatic reclamation.
The actual material-IR declaration precursor and optional generated-pixel
holding/staging path prepare paced registration; they do not yet split the
final DayZ resolver. After documented compile-name repairs, game/tests/tools
and397 assertions/17 focused cases passed. Installed automatic repeat-route and
real generated TextureBank diagnostic remain pending. Whole plan OPEN;
defaultReady=false/performanceAccepted=false, no Nanite equivalence claim.

### 30 September — installed automatic traversal and next material seam

Exact `9ec4937c` passed paired startup and the automatic/generated runner
`hierarchy-auto-generated-sep30-20260930-010540-46ce2e`, exit0/normal without
forced termination. Far/near/far/near/far reused root64/model3 and Fine512/model4,
with reads1/2/2/2/2 and two refinements/two coarsenings. Generated TextureBank
staging uploaded once under its owner stage, reused the handle, and detected
an ordinary early consumer while preserving that consumer's normal upload.
Final pending generated charge was0; no retail DayZ capture scope follows.
Next source introduces an exact-tenement default-off pure classifier tail
from live owner facts; its face upload remains synchronous. It also fixes the
DDC key to hash the exact FileServer parser bytes. After replacing an identical
test fixture, the proper Core suite passed3,564 assertions/12 cases and the
classifier suite passed31 assertions/5 cases. Installed DayZ verification is
pending. Whole plan remains OPEN/defaultReady=false/performanceAccepted=false;
retail source leases, complete final material binding, paced owner registration,
physical convergence and comparable-quality performance remain required.

### 30 September — actual source precursor and pre-upload birth capture

Installed `f62e5253` passed startup and DayZ prepared-classifier ON/OFF traversal,
both normal/exit0 with returned19,923 resident/zero pending. ON classified34
actual tenement sections; OFF emitted none. Ordered p95 frames126.680/106.170ms
do not establish an improvement; feature remains default-off/performanceAccepted=false.
Next source retains actual pre-upload texture refs/Init birth leases in a bounded
selected-owner observer and adds a supplied-role dependency packet; it does not
yet pace complete retail registration. After a missing-include build repair,
dependency/birth/candidate checks passed5,228 assertions/18 cases.

An additive128KiB Cprs lease preserves original raw-source refusal rules. Real
installed skala_new.p3d decode/SHA/ODOL7 checks passed19 assertions; independent
wire/Tools inspection and canonical raw-IR gate found four rigid visual LODs
and one PAC opaque across all seven mips. This advances actual retail source
inspection but not finalized SVertex/config/object/shadow/pass admission. Normal
physics geometry, Roadway5 and fire/view6 must remain conventional. Installed
observer/native verification is pending; whole plan remains OPEN/defaultReady=false,
performanceAccepted=false, with no Nanite-equivalence claim.

### 30 September — native lifecycle and birth-observer test boundary

Exact installed `1de1caa5` passed paired WGPU startup. Normal native Reforger
`cancel-20260930-015556-texture-keys-1-authored-far-0-lazy-bcr-default-bc7-default`
completed normally/exit0 with a valid capture and no contention. The recorded
8.376ms frame was taken with main0/zero main triangles, so it is not comparable
quality, an FPS improvement or a no-regression proof. Native compatibility is
still not rendering ground truth.

The first selected DayZ birth-observer route captured37 actual unique refs:
33 physical and4 generated; all remained resident and the33 physical bindings
remained current. Its overall runner failed an independent warm-upload witness.
The source-only follow-up was a genuine negative capacity case: the unchanged
256-binding pool was exhausted before the selected model, with6,246 refusals;
only4 generated refs were captured. Neither failure is reclassified as a pass.
The runner now uses retirement to exercise birth capture and reports independent
warm-upload evidence separately; explicit Require* checks remain enforced.

Next source adds a bounded scoped final-binding guard before upload mutation,
with unchanged resident-handle/slot reuse and refusal of missing/deferred/new
images. It does not intercept material Load/Init or prove complete roles. A
strict finalized ODOL7 source-pair exporter verifies real MeshBuild packing,
source lease and owner-supplied live material facts; source0/1 cannot replace
original source2/3 shadow paths. Build-name/include repairs are retained in logs;
installed verification is pending. Retail pacing and automatic geometry paging
remain OPEN/defaultReady=false/performanceAccepted=false; no Nanite equivalence.

The next combined source passes451 assertions/11 focused reuse/final-export/
receipt cases,41 assertions/2 fresh opt-in PAC actual-read/Init cases, and3
assertions/1 fresh default-off purpose case. Compile repairs corrected a
missing header, ambiguous test names, RString conversion and C-array identity
copy; failure logs remain. An exact native-bank owner capture now reaches the
canonical parser and actual live adapter tail from the same decoded ODOL bytes.
The PAC lease is born only in its original raw read under an exact default-off
purpose and retained by successful Init; it does not retrofit cached unknowns.

The private source probe stages its sole actual primary texture, then verifies
144/250-triangle finalized MeshBuild payloads under a reuse-only guard and live
material/slot checks. This is explicitly FinalShape/Init provenance, not exact
GPU pixel-source provenance, global VFS/world admission, pass replacement or
paced retail registration. ModelRegister now supports a bounded optional drain
receipt; the private hierarchy gates fallback/cut instances on consumed accepted
receipts with actual owner/source epochs. Null-receipt ordinary operations retain
the existing path, while invalid renderer IDs cannot become instances. Receipt
consumption does not settle retirement debt. Installed verification remains
pending; no performance/default-ready/Nanite claim.

### 30 September — installed retail probe, receipts and normal DayZ controls

Exact installed `99a200a0` passed paired WGPU startup and the combined
`retail-final-reuse-receipt-sep30-20260930-023744-0a0740` runner normally/exit0.
The real native ODOL7 source yielded 144/250 finalized triangles; a separate
controlled hierarchy repeated root64/Fine512/root64/Fine512/root64 with
accepted receipt counts2/3/3/3/3, no rejected/pending receipt, and reads1/2/2/2/2.
The actual generated reuse guard allowed1, denied2, and preserved both owner
creates/retries. This did not render retail pages or prove PAC GPU pixel birth.

Normal disabled DayZ controls before/after (`default-control-1de-sep30-disabled-
20260930-023330` and `default-control-99a-sep30-disabled-20260930-023938`) both
exited normally with zero dropped trace frames. Median22.663/23.053ms,
p95122.366/98.625ms and p99354.190/282.018ms are ordered-cache observations,
not a causal performance improvement or comparable-quality acceptance.

The birth/retirement route `birth-retirement-99a-sep30-disabled-20260930-024331`
exited normally and captured28 current physical plus4 generated refs, but its
result.passed remained false: the JSON verdict still required an independent
warm-upload witness despite marking it optional for the observer. That failure
is preserved; next source fixes the verdict expression and will rerun once.
The observer remains partial (63 missing-source observations), role closure false.

Next source connects fresh private PAC Init, pure leased BC1 preparation and
one exact source-bound upload to the real final-shape exporter, then builds
independently hashed GHP pages. Actual alpha facts come from those held blocks.
The new diagnostic retains material/source identity and stages non-instanced
page/model records with drain receipts and complete generational record checks.
It intentionally does not replace world draws or shadow passes. Defaults remain
unchanged; whole plan OPEN/defaultReady=false/performanceAccepted=false.

The next source game/tests/tools build passed. Focused producer/reuse checks
passed339 assertions/11 cases, exact record/model receipts68 assertions/5 cases,
and fresh opt-in PAC preparation17 assertions/1 case. Two initially supplied
Catch filters matched no cases; corrected receipt/record filters ran separately
and their logs are retained. Independent review repaired an unreachable
Failed-state cleanup branch; exact postcommit runtime verification is pending.
The runtime pilot stages at most one mesh per owner frame, authenticates every
independent page, retains fallback/root coverage as records only, and joins its
worker before deleting an owned CREATE_NEW file. Capacity figures measure known
owned storage; producer temporary peak, allocator/RSS and device completion
remain unproved. No world replacement or performance/default promotion.

The native file/teardown refinement build passed and the independent hierarchy
codec/reader suite passed12,210 assertions/7 cases, including held DELETE-access
read and exact-handle deletion with a preserved neighboring sentinel. Windows
reads now share the producer's existing accesses while that producer still denies
other writers/deleters. Teardown headroom is reserved before admission; reports
carry actual worker debt, and historical mesh preflight counts the root/fine page
union plus refill IDs. Installed verification remains pending.

### 30 September — actual retail records and a strict refill failure

Exact paired installed `42f98d4b` passed startup. The first retail runner
`retail-pixels-pages-sep30-20260930-033204-e70a1b` reached actual Fine records:
144/250 source triangles, two independent pages, eight Present meshes and three
accepted models, no worker debt. The PAC raw SHA was83d60c398c7fd2ebec3cafc31407021a2d120a675c097013f9a19eb173762769.
Actual initialized upload used six levels/43,680 BC1 bytes from the43,839-byte
source; alpha derived from65,536 top-mip pixels had zero clear pixels. This is
upload-byte provenance, not raster/pixel or complete world/material authority.

Release observed four old detail records Absent while retaining four. Refill
created four fresh meshes plus one fresh model, but its latest confirmation
covered only the selected new cut, omitting old Absent generations. The runner
correctly failed its full-history check; normal shutdown/exit0 occurred without
forced termination. That failed result remains preserved. Next source adds a
full-history observation before every Ready; acceptance requires the rerun.

The corrected birth-observer runner `birth-retirement-42f-sep30-disabled-
20260930-033415` passed normally/exit0:32 current physical refs plus4 generated,
all36 resident;47 observations still lacked source birth, role closure false.
Independent warm-upload evidence stays separate and optional for this observer.

Next source also adds an explicit retained-instance main-camera-only flag with
positive main-view authority, so unknown/shadow/reflection/sky/GI culls exclude
it. COUNT keys include that authority. No world/CPU suppression or page instance
is enabled yet; focused actual-GPU validation and installed rerun are pending.
Whole plan remains OPEN/defaultReady=false/performanceAccepted=false.

The full-history Ready repair and main-camera-only source build passed. Actual
GPU COUNT plus indirect-record validation passed for main and the separate color
cull, with the page excluded from solar/local shadows, reflection, sky and GI;
ordinary control instances survived every view. The COUNT key test also passed.
Normal installed42f control `default-control-42f-sep30-disabled-20260930-034047`
passed with921 frames, zero dropped trace frames and normal exit0/no force.
These checks do not enable retail page instances or prove a world replacement.
The corrected full-history installed rerun remains pending.

### 30 September — complete retail history accepted, visible pilot pending

Exact paired `b911afab` startup and `retail-full-history-b911-20260930-035017-dcc50c`
passed normally/exit0. Initial Ready observed8 Present; release4 Present/4 Absent;
refill8 Present/4 historical Absent with fresh IDs; final abort12 Absent meshes,
all4 model maps absent, worker debt0. Separate controlled automatic traversal
also passed. The earlier42f refill failure remains preserved.

Normal DayZ controls42f/b911 both passed/no dropped trace frames, median
22.921/22.612ms, p95108.242/113.111ms, p99354.061/339.959ms. Ordered-cache
screening is not a causal performance benefit or acceptance.

Next source adds an explicit default-off, bounded actual-retail private visible
instance. A literal generation-bound CPU row fact and immutable operation receipt
join add/switch/remove to a fresh actual returned main camera. Root-before-retire
and remove-before-abort require separate owner frames. This private copy does
not preserve/prove same-placement original other-pass geometry or replace world
objects. Pure receipt checks passed187 assertions/6 cases; installed verification
is pending. Whole plan OPEN/defaultReady=false/performanceAccepted=false.

The visible source build and187 assertion/6-case receipt suite passed. Three
pure Rust actual-table/ABI tests passed. Review repairs freeze epoch evidence
AFTER the whole instance queue, preserve exact consumed rows for cleanup when
owner source/camera validation fails, and allow at most three new-ticket
idempotent historical-Absent Remove observations. No stale Remove FFI is issued;
terminal rejected receipts never become visible Ready. The maximum12 instance
operations are reserved before visible admission. Installed visible verification
remains pending; normal default paths do not query instance facts.

### 30 September — actual visible lifecycle, material fidelity repair next

Exact317ec996 installed startup and `retail-visible-317-20260930-041333-bdfe77`
passed normally/exit0: one literal instance handle3 through source coarse/root/
Fine/root/refilled-Fine, then returned historical removal. The images positively
show rock presence and absence, but BOTH the source-coarse reference and paged
Fine appear nearly black. This is not visual/material parity acceptance.

Read-only audit identified a real private material-tail omission: scalar-only
prepared classification retained zero layer UV/tint, while the ordinary
ResolveMaterialLayers tail sets identity. The shader multiplied albedo by zero.
Next source runs that actual ordinary no-surface tail under the frozen reuse
scope and rejects nonidentity unlayered admission. Lighting/PAC bytes unchanged.

Next source also uses an independent actual source Fine reference (250tris)
only under private-visible gate, preserving record-only coarse144. Complete Fine
page roundtrips now compare oriented original-ID triangles and multiplicities,
plus scalar vertices, with bounded/charged scratch. Camera generation is reported
separately from operation frame attempt. Runtime quality acceptance remains pending.

Normal317 DayZ control passed/zero dropped/normal exit0, median22.587ms,
p95123.927ms/p99360.358ms; variation versus prior b911 is not causal acceptance.
Native Reforger Village317 captured4,930 main instances/13,451,410 main triangles,
31.200ms sample, normal exit0/uncontended after79.57s. This is a populated native
streaming lifecycle observation, not a quality ground truth, improvement or
matched no-regression proof. Whole plan OPEN/defaultReady=false/performanceAccepted=false.

The source-Fine/material game+Core+Tools-dependent test build passed after one
Catch2 compound-assert parentheses repair (failure log preserved). Fine-product
checks passed195 assertions/8 cases, including oriented parity rejection cases.
The visible runner now records repeated source-Fine/Fine/source-Fine images with
jitter/upscaling and grass/cloud/planar ablations isolated to this test process.
No image registration or quality acceptance is inferred from the triangle proof.

### 30 September — actual Fine/material comparison and cold-world seam

Exact05cdceed installed startup selected matching EXE/DLL and Tidewater Native.
`retail-fine-material05cd-20260930-043441-420afb` passed normally/exit0,
including sourceFine250/root/Fine/sourceFine/root/release/refill/remove and fresh
returned camera generations146..338. Images now show the actual PAC texture:
the ordinary no-surface material tail fixed the private black-albedo defect.
Initial repeated images drifted with autoexposure (source-repeat MAE7.977,
source/Fine13.296, source-again16.028 on fixed450x550 rock-interior ROI); no
pixel acceptance was inferred from that run.

Supported `WGR_AUTO_EXPOSURE=0` is now isolated to RetailVisible captures.
Same installed05cd source `retail-fixed-exposure05cd-20260930-044420-eed96d`
passed normally/exit0; no registration/alignment applied. Fixed ROI[400,100,
850,650] source-repeat MAE0.001313, Fine-repeat0.010661, source/Fine0.012983,
source-again0.039506; every comparison maximum absolute channel difference1
(8-bit). This is narrow same-placement source-Fine/paged-Fine material/geometry
image consistency, not world takeover/all-view/coarse-quality/performance proof.

Complementary OTHER_VIEWS_ONLY bit64 is source-implemented, default unused.
Actual-GPU COUNT/indirect-record test passed1 case for page32, original64,
ordinary0 and conflicting32|64 across main/color and solar/local/reflection/sky/GI.
Unknown main authority preserves original64; conflict is invisible in all views.
No CPU coverage/shadow LOD policy is changed or certified by this flag alone.

Next source captures one selected physical Cprs read through the actual
FileServer parser buffer and cold canonical Shape creation, with one-shot
read-scope claim and a nonwrapping Shape birth. Cached/preparsed/pre-adapted,
DDC-only, deferred, reversed and unbound loads remain Unknown. Exact selected
cold DDC bypass is private/default-off; ordinary cache policy unchanged.
The actual bank-owned primary PAC is prepared/uploaded before retained model
registration only if cold Init/layout/source proof exists. Already warm/unbound
textures refuse the private observation and retain normal world behavior.
Actual world placement telemetry is queued producer evidence only: no renderer
return, replacement, shadow/material closure or generic Nanite authority.
Build/runtime validation of this next seam is pending. Whole plan remains
OPEN/defaultReady=false/performanceAccepted=false.

Cold-world source build passed after correcting one log-helper namespace name;
initial failed build retained. Actual Cprs/one-shot/changed-member/cap checks
passed160 assertions/4 cases; Model DDC checks3568/12 and logical Shape checks199/12
passed. Two initial test invocations matched no cases and were corrected to the
actual target/tag; no zero-test result counted as validation. Normal scene/world
hooks freeze the launch gate once, adding no per-object getenv/query/IO in OFF
mode. Actual cold-world installed runtime is still pending.

Exactf4ccb276 paired startup passed with matching build provenance/Tidewater.
First cold-world driver failure is preserved (`retail-cold-world-f4ccb-20260930-
051333-1d3b1f`): async screenshot was checked before readback finished. Engine
already emitted actual parser/Shape/PAC and two current ordinary queued placement
facts; cleanup exited0 normally, no force. The driver now waits bounded10s for
readback and centers its camera on the unchanged placements.
Same installedf4 repaired `retail-cold-world-f4ccb-repaired-20260930-051707-eaca6a`
passed normally/exit0/no force: actual Shape birth1,53786 decoded bytes,4 visuals,
P3D SHA1048fad...ecb78b, bank-owned PAC handle4294967316/slot20, raw83d60c...762769,
uploaded4b6a89...47229, two ordinary queued placements0/1 sourceCurrent/static/
intact=true, flags0. This is actual cold-world source/primary ownership evidence,
not renderer-return or world page takeover.

Normal f4 DayZ OFF control passed1259 frames/zero dropped/normal exit0,
median22.702ms/p95127.600ms/p99336.965ms. Ordered cache/history and variable
first-touch timing prevent causal performance or no-regression acceptance.
No headless GPU job or second game overlapped this capture.

Next source exports a candidate and pages from the SAME installed Shape birth,
retaining all original levels/passes/model. Actual ordinary material facts and
classifier are reused through member wrappers; no hand-authored sampler/tint.
This record-only action requires explicit cold-source, fixture and frozen-reuse
gates; no private replacement Shape/PAC or world-instance operation. Candidate
refusal/capacity checks passed20 assertions/3 cases and game+Core build passed.
Installed actual-world record lifetime remains pending; whole plan still OPEN,
defaultReady=false/performanceAccepted=false.


### 30 September — actual cold-world page-record lifetime and paired-row admission

Exact82c2b179 paired build/startup passed. Installed
`retail-cold-world-records82c2-20260930-053645-a6a207` passed normal exit0/no force:
actual world source export, Fine250 oriented parity, records ready8mesh/3model,
release4present/4absent mesh and2present/1absent model, refill8present/4absent
mesh with freshIDs16, abort0present/12absent mesh and0present/4absent model.
Worker debt0. Original world model/placement remained unchanged; no page instance
or world coverage takeover in this run.

Native Reforger Village82c2 passed1/1 populated captures, normal exit0 and
uncontended:20,683 registered/4,930 main instances,13,451,410 main triangles,
31.121ms sample after79.4s. Prior317 sample31.200ms is an ordered historical
comparison, not causal performance acceptance or material-quality ground truth.

The next pure paired-row transaction passed463 assertions/8 cases. It requires
literal actual original/page before and after rows, nonrecycled consumer births,
full model/renderer mappings, one drained instance epoch and a fresh actual
returned camera joined bitwise to the submitted packet. Partial mutation,
wrong binding, stale camera, skipped return and alias facts fail closed. This is
CPU row/returned-camera proof, not pixels, GPU completion or source authority.
Actual world producer/consumer integration and installed runtime remain pending.
Whole plan OPEN/defaultReady=false/performanceAccepted=false; no Nanite parity
or generic geometry streaming acceptance inferred from this bounded pilot.


Actual-world producer/consumer source now builds with the normal game target;
final pure pair checks again passed463 assertions/8 cases. One explicit selected
ordinary Add carries its nonwrapping object birth; ordinary move/remove/reuse
first rolls back the exact source/page pair, then clears the old consumer birth.
The original normal model is retained until returned page absence. Source-only
invalidation restores a still-current original before private page cleanup;
unknown hidden rows retain their resources. Added bounded generated-fixture
matrix-move/remove actions exercising the actual landscape hooks. The driver
captures original/Fine/Root/refill/Restore and supports isolated lifecycle runs.
Installed transition/quality/lifecycle validation of this source is pending.


Installed83 pair driver failure is preserved (`world-visible83-20260930-061734-
9cd22a`): the main-camera tracker was disabled by its older surface-certificate
launch gate, so pair/cleanup camera receipts rejected; normal shutdown/exit0,
no force. A driver-only attempt to enable that gate exited2 normally at startup:
existing application correctly demanded a private original/surface fixture.
Neither refusal counts as passed world transition or camera evidence.
Next renderer source independently enables the existing camera tracker under
explicit fixture+actual-world-visible+camera gates, without advertising a
surface certificate or triggering the private original-fixture startup action.
