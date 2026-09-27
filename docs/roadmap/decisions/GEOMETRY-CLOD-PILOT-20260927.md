# Controlled resident CPU CLOD pilot

The repaired build and focused CPU checks passed at the 11:06 installed 86971cce
checkpoint. CLOD bake and full-DAG-cut decoding link only into Tests; the game
submits no CLOD output. A separate opt-in authored-LOD resident fixture passed
runtime checks. Normal defaults, gameplay geometry and streaming workers remain
unchanged. This is partial Stage G evidence, not completion.

The dependency is upstream meshoptimizer at
[`9e1f07b159d3cb777f1c67ed31fc11fd117986f4`](https://github.com/zeux/meshoptimizer/tree/9e1f07b159d3cb777f1c67ed31fc11fd117986f4),
including its `demo/clusterlod.h` example. The minimal CPU dependency retains its
MIT notices. It imports no NVIDIA renderer, RTX SDK, Vulkan sample or assets.

`GeometryPageClodBake.hpp` accepts the immutable fine representation of a
controlled rigid, single-material `ShapeExport`. This caller certificate excludes
animation, skinning, forest/conformance, mutable proxies, destruction and unknown
material semantics. It is not arbitrary retail-model eligibility proof. Positions
must match the exact packed `SVertex` records. Normal and both UV attributes are
passed to the simplifier; all coincident original vertex IDs are locked. Output
indices reference the original unchanged attribute buffer. Permissive, fallback
permissive/sloppy, dilation, regularization and error-clamping policies are disabled.

The fixed policy, weights, locks, supported numeric domain and validation rules
belong to `AdapterVersion=1`; changes require a version bump before reuse. A future
cache key must also identify the exact library revision, immutable source witness,
selected representation, vertex layout and material mapping. No disk cache format
or serialized-data parser is implemented by this checkpoint.

Input caps are 4096 vertices and 8192 triangles, with finite local coordinates and
simplification attributes bounded to absolute value 1,000,000. Captured output is
limited to 4096 groups, 16384 clusters, 1,048,576 index entries and 8MiB logical stored
contents. Each cluster is limited to 64 unique vertices and 128 triangles. These
limits exclude vector capacity, allocator metadata and external library scratch.
One upstream call can allocate its own vectors and run without an interruptible
budget. The callback returns group IDs; refusal stops output copying and discards
the transaction after the call, and does not cancel the bake. This is unsuitable
for a live worker/owner-frame integration until separate resource and lifecycle
gates are implemented.

The adapter stores paired containing-group/refined-group IDs, group bounds and
simplification errors. It validates finite bounds/errors, prior-group references,
monotonic group errors, terminal roots and exact winding-preserving original-leaf
triangle coverage before publishing. Cluster-bound errors may be nonmonotonic and
are not selection criteria. The resident reference cut uses both group predicates
and replaces entire refinement groups. `SelectClodCut` requires an immutable,
successful `BakeClodPilot` result; its topology checks do not independently certify
mutated index payloads or externally constructed geometry.

Focused `[geometry-clod-bake]` source tests exercise a real indexed planar grid,
original attributes and seam IDs, refinement output, exact leaf coverage,
transactional input/output refusals, malformed topology and a hand-constructed
paired-error cut. The focused suite passed 30,600 assertions in 60 cases without
skips, including the later curved positive-error fixture requiring distinct fine
and coarse selected IDs/triangle counts and exact CLOD cut-buffer decoding.

The next CLOD renderer gate is a resident conventional indexed-draw pilot through the
existing resource queue, with original authored/query geometry retained. It must
prove complete cuts, material/attribute equivalence and consistent required passes
before disk backing, asynchronous requests, eviction/refill or pressure handling.
Draw counts, actual allocation/staging/in-flight bytes, loading and frame tails
need matched quality/work measurements. No performance acceptance, physical
reclamation, Nanite equivalence or completion of the streaming plan is claimed.

## Actual build, resident fixture and repeat-route evidence

Sources 29c926fe (pinned CPU CLOD/export/decode), 45e7e5f9 (complete CLOD cut DTO),
0f4a6908 (authored resident draw fixture) and 8b42afdb (narrowing repair) are compiled
in installed 86971cce. Later 86971cce/f16f60bd runner records ownership cuts at
returns 2/3 and keeps traversal pruning acceptance at the final evidence cut.
Installed executable SHA256 is
`c55c1377648d33044b71214279be90221d63642fe830ad46ea63bbe900b90a02`;
renderer DLL is
`87a72c68631add5d997e7ab11c9c753f875c0c8594ee599a037e5a24c0c23998`.
Original provenance distinguishes each runner/checkout head from that binary stamp.

The initial build failed a fixture fact-count narrowing conversion; its log is
retained below. The repaired five-target build completed 396 scheduled steps.
The focused suite passed 30,600 assertions in 60 cases without skips. CPU CLOD
checks include a curved positive-error shell with different fine/coarse cut IDs
and triangle counts. They prove bounded adapter/cut contracts, not runtime CLOD,
bake/loading throughput or FPS acceptance. Later CLOD game-fixture work is unbuilt
and excluded from this installed checkpoint.

`authored-page-resident-20260927-20260927-110657` passed with cleanExit=true.
Its programmatically authored Shape source is neither the generated MLOD file nor
meshoptimizer CLOD. The process-local DTO witness is
`c16d8df52ea9c579df38168d295090d444c868b906b94f37182feba5c5bbd578`.
The payload was 74,992 known logical bytes/three meshes, with complete
12/48-triangle representations switched on the same instance. Three generational
records were Present and subsequently Absent. Geometry/Fire/View helper hashes
stayed `66aac0a97a61c4936fe47486ab1af12483a34bc6b154322d5a0f3c6429bfd09f`.
Root inspected coarse/fine black-box silhouettes. A returned main frame and
configured cascade do not prove material/pixel/depth/shadow/reflection parity;
local shadows were not configured. Record absence is not queue completion,
device-memory release, full allocation ownership or reclamation.

Disabled authored control 110746 passed with cleanExit=true and zero fixture
payload/meshes. Private standalone 110848 and shared 110933 regressions passed and
logged normal shutdown. Standalone handle 4294967357 went Present-to-Absent;
shared newest-first/producer-first cases used handles 4294967357/12884901949,
retained Present with a remaining borrower and ended Absent at zero borrowers.
These copied cuts do not prove world rendering or physical free. The known witness
alias may outlive a dropped mesh; nonzero witness metadata at that cut is expected.

`ownership-returns-20260927-20260927-111103` passed/Converged and exited normally.
Original resource acceptance thresholds were unchanged; lateQuiet and retiredClear
were true. Geometry changed 727,567,316 to 723,573,800 bytes (-3,993,516); capacity
stayed 1,275,068,416 and retired bytes stayed zero. Object textures changed
2,766,830,964 to 2,758,268,116 (-8,562,848), count 2518 to 2490; backend allocation
changed 5,323,900,496 to 5,316,052,240. Separate post-quiet diagnostic cuts had
4109 to 4099 live records/tracked allocations, full logical closure/residual zero,
and model-LOD links 1920 to 1910. CPU referenced vertex/index bytes remained
544,174,964/77,212,116; retained shared bytes 177,468,916/27,653,568 and standalone
bytes 93,570,040/13,495,620 also stayed unchanged. These categories can overlap.

Both asset/LOD reports were truncated. Their sampled unique vertex/index unions
increased 1,461,456/272,832 bytes while the whole pool decreased. This directly
shows why sampled occurrence/union deltas cannot establish whole-pool attribution
or exclusivity. Cuts occur after quiet samples/timing, with an extra bounded
inter-route interval; they are not atomic with preceding process/resource samples.
The previous ResourceGrowth failure (+15,338,464 bytes against the unchanged 8MiB
allowance) is retained. This one repeat does not prove universal convergence,
process-memory stability, pressure handling or causal performance/no-regression.
The overall plan stays OPEN, performanceAccepted=false/defaultReady=false, with
complete ownership/reclaim gates unresolved and experiments off by default.

### Frozen artifact SHA256

Paths are relative to the worktree. Original files are authoritative; this compact
manifest does not embed their full JSON telemetry or claim future artifact results.

| Artifact | SHA256 |
|---|---|
| build/build-resident-clod-fixture-20260927.log | 7712d1d27af9e16a85e6c83d32e1300c132910518ee4e3098fe045c4b406e05f |
| build/build-resident-clod-fixture-repair-20260927.log | 22fe89b86d2e44e475c095df7c12e5bbfadd814eb3477b672fda29f01df361ba |
| build/test-resident-clod-fixture-20260927.log | bbbf3a66495e92d284654102bf283a00b642206361f011026155173b5cb0ba53 |
| build/stream-residency/authored-page-resident-20260927-20260927-110657/coarse.png | c6136f320df9f73a864edf3c014d979e00c9c3526ff7fb5aa650df779b961f32 |
| build/stream-residency/authored-page-resident-20260927-20260927-110657/engine.log | ef6210222f65553fef23c73267b02b46a179acf8f6712ecaf4547d09a3508d24 |
| build/stream-residency/authored-page-resident-20260927-20260927-110657/fine.png | 60d3145bdd4f8a5b6b9d3aa322da058a2a9103ed50b47fd37398c46a06d79210 |
| build/stream-residency/authored-page-resident-20260927-20260927-110657/harness.jsonl | 24ad24cf098a5370579ef6cf51dcd6a8f572982c741c96ec046ee1f43584197d |
| build/stream-residency/authored-page-resident-20260927-20260927-110657/provenance.json | 0a66dd64b82c0fab2df95190aadbab3ce6aa1c3e1dd649b7a8b4249d6014ca43 |
| build/stream-residency/authored-page-resident-20260927-20260927-110657/result.json | c737f98ed10c2b92ef8004ecef70f334b2426f2cfc4c205090c834c62b723653 |
| build/stream-residency/authored-page-disabled-20260927-20260927-110746/engine.log | 4162094a526f163839767f71c1e7a5a2ed2d89c965dc767513cd302f6780990c |
| build/stream-residency/authored-page-disabled-20260927-20260927-110746/harness.jsonl | 38af72d9b76c30025ceb5cbb60ed5b02820bd92a13176bd2f47484c2525bdf46 |
| build/stream-residency/authored-page-disabled-20260927-20260927-110746/provenance.json | 23342c3dcea43fd6a7d68614c078f4713e790f78ae6ee3877959f3b6d8afdc5f |
| build/stream-residency/authored-page-disabled-20260927-20260927-110746/result.json | 4bf8ea31cb1a9f79efea047038700949e5e7576f43e3f0234998f26057674c26 |
| build/stream-residency/resident-payload-standalone-20260927-20260927-110848/engine.log | e06ac8041da7e3f637f3fefff7ee5a1748290ab0db7dd74176e4091936dbccc8 |
| build/stream-residency/resident-payload-standalone-20260927-20260927-110848/harness.jsonl | d4c9aad40c569b77754b0439ba97ea0315fcd2cb29fe8a4fe2dd591c1ababdaf |
| build/stream-residency/resident-payload-standalone-20260927-20260927-110848/provenance.json | 048fbb44a11d38335341b5d738d6c784f635c3a28982e11a0aebd4d32b2c0559 |
| build/stream-residency/resident-payload-standalone-20260927-20260927-110848/result.json | 280a232c5b32d64aa0bfa62f5e9c778624204c19763f758bad20c958501a806c |
| build/stream-residency/resident-payload-shared-20260927-20260927-110933/engine.log | 20e5375cdfcc764ef63bf43b0dfbcd2d573053e18bbd32dc9bc7f7f848b9dbbc |
| build/stream-residency/resident-payload-shared-20260927-20260927-110933/harness.jsonl | 974f9895ee05dd8c88303979eff8c9d067e81adc9a0c7f787c1aac54e37f8692 |
| build/stream-residency/resident-payload-shared-20260927-20260927-110933/provenance.json | fd247cd66856e23fda0aba5d70204001619dff9d6a32d3e27218db12f62e6a72 |
| build/stream-residency/resident-payload-shared-20260927-20260927-110933/result.json | a13ace822740060a168e80fa0a976606df889607c3d40377e1e1f4db0e407dad |
| build/stream-residency/ownership-returns-20260927-20260927-111103/engine.log | ce74528aa9bd0e572352f8051dbf79614b9ccdda61338526d62be6649fa9bf33 |
| build/stream-residency/ownership-returns-20260927-20260927-111103/geometry-allocations-bounded.json | 51872ed50d475ef21e27f9d447ec9ade3ad8f58f3998fb3b28dab027d490bef7 |
| build/stream-residency/ownership-returns-20260927-20260927-111103/geometry-allocations-return-2.json | 7955edd5c00c8d92d66bf4551adfefc894f18eb7223eb1aea5a1494ef67069cb |
| build/stream-residency/ownership-returns-20260927-20260927-111103/geometry-allocations-return-3.json | 27d2b571dc7d832fc9049e866d2850ca4ee9264ea3574a56f83b65298b16d9fd |
| build/stream-residency/ownership-returns-20260927-20260927-111103/geometry-allocations-return-comparison.json | d9fbd8067ce5c6605f41649668d5abc388fee6711566ee8e6873b2e73c813c65 |
| build/stream-residency/ownership-returns-20260927-20260927-111103/geometry-allocations.json | 9047546deb91e2e80dc4dc98c7d2dddab9279ef1fc642bd95f6ec0758d264f14 |
| build/stream-residency/ownership-returns-20260927-20260927-111103/harness.jsonl | 646c935d72491559744819e802f1eb7e8c7bb2950de70b015ee5aca1b6ce7391 |
| build/stream-residency/ownership-returns-20260927-20260927-111103/provenance.json | 38af557b5b7c9af8fcf2db212257eda591ea9b31b4624ff9501887975aa8ef01 |
| build/stream-residency/ownership-returns-20260927-20260927-111103/result.json | ce75f3167d560d731784170142c5ee08365a6c74ad655309bbb8f7cbc03a63a1 |
| build/stream-residency/ownership-returns-20260927-20260927-111103/returned-settled.png | 010f509d263d362e955f7e9ba1ed8878f6acfceddb44d34b2f2ccb5a4d19484e |
| build/stream-residency/ownership-returns-20260927-20260927-111103/samples.jsonl | 8b4b16516a24608ba40fa8ed2ad443cb0f1184b92c505a80eeb6410e0bea3237 |
