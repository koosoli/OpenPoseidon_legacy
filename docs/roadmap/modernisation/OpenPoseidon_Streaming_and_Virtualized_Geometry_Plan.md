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
