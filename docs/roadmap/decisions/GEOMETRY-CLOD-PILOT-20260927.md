# Controlled resident CPU CLOD pilot

Installed clean c072453c passed the explicit combined selected-CLOD RAM worker,
independent fallback, one-cycle fine eviction/refill and cancellation-debt checks.
Default GPU table uploads now use verified dirty domains; traversal performance
for this new mask remains unmeasured. Earlier51 screening was mixed. The startup
fixture gate remains off; normal frames perform no bake/page work. Stage G/full
plan remain OPEN, without performance/default or physical-reclaim acceptance.

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

The first resident conventional indexed-draw pilot now passed narrow runtime
checks. The remaining CLOD renderer gates use the
existing resource queue, with original authored/query geometry retained. They must
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

## Actual resident CLOD checkpoint, 11:18 installed bdb02bb8+dirty

The five-target build passed 257 scheduled steps and the focused cut/bake/complete
representation suite passed 6,058 assertions in 11 cases. The installed label is
`bdb02bb8+dirty 2026-09-27_11:18 deploy-ps1`: only these documentation changes were
dirty at deployment, subsequently committed as e928; functional source is bdb.
Executable SHA256 is
`5863803b7362dc3bdc427d72e08b6986a314b062e372ce79fa97287a24bc1108`;
DLL SHA256 is
`f1cadf9fc4f3bc2a8afcd4e773641fcc1d59ecfe27f948c7ec0d87f70721fea2`.

`clod-resident-draw-20260927-20260927-111920` passed with a clean exit. Explicit
`BeginClod` synchronously baked a controlled curved grid into four groups and
12 clusters, then decoded paired coarse/fine cuts of 64/512 triangles. The original
authored fine representation also has 512 triangles; the separate authored fallback
has two. Coarse threshold was 0.09406139701604843, fine threshold zero. Three distinct
models switched on the same renderer instance (1). Eight actual mesh records were
Present and later Absent. Known stored fixture bytes were 351,017; this excludes
complete temporary scratch/peak allocation. The one explicit local bake took
1.1831ms, which is neither an owner-frame budget nor a performance bound.
CPU helper hashes remained identical. Meshoptimizer is now linked into Renderer
for this explicit operation, not an automatic frame/worker bake path.

The disabled CLOD control `clod-resident-disabled-20260927-20260927-112035` passed
with a clean exit and zero fixture payload/meshes. The common publication regression
`authored-after-clod-20260927-20260927-112142` also passed with a clean exit:
whole authored 12/48-triangle cuts, three Present-to-Absent records, unchanged helper
hashes and 83,896 known stored bytes. This later accounting value must not replace
the earlier 74,992-byte checkpoint retrospectively.

Root viewed actual coarse/fine curved black silhouettes. Main-frame return and
configured cascades are observed; local shadow was not configured. These views do
not certify pixel/material or all-pass equivalence. Source witnesses are process-local
controlled authoring, not mounted-file or arbitrary retail eligibility evidence.
No async paging, disk backing, eviction/refill, physical reclaim, full ownership,
performance acceptance or normal-default promotion follows. The full plan remains
OPEN with performanceAccepted/defaultReady false. Later candidate patches are not
included in this installed checkpoint.

Actual checkpoint SHA256 manifest:

| Artifact | SHA256 |
| --- | --- |
| build/build-clod-draw-pilot-20260927.log | ac525dbdf51c4c8756978025c406e48a97175acea4dd6090b2f347f0d65e8880 |
| build/test-clod-draw-pilot-20260927.log | 6cbe92e122c61ba99de69ee577abf55c64c7b7865276e2a753626ae283ec83e7 |
| build/stream-residency/clod-resident-draw-20260927-20260927-111920/coarse.png | e2cbde8192e0f0bb30bdab3c7c91d499365665cc38a9cdfd23210da188e4531e |
| build/stream-residency/clod-resident-draw-20260927-20260927-111920/engine.log | ba74bb46f3bb2a263e666c3d81e74506ae7bac62dc8c52170e61c7a8011675cf |
| build/stream-residency/clod-resident-draw-20260927-20260927-111920/fallback.png | 0adfed32d4e2e33217dde6dcb46d44761406957e279c3dc077051ac0feb8720a |
| build/stream-residency/clod-resident-draw-20260927-20260927-111920/fine.png | 7d419edfe0cb97ffb7f0b688064b42a3434724524843175d6d72ab687b480173 |
| build/stream-residency/clod-resident-draw-20260927-20260927-111920/harness.jsonl | 2718fbbbba29947f64003393914d0b41e3709bf966da0f2f8729617285f2c88f |
| build/stream-residency/clod-resident-draw-20260927-20260927-111920/provenance.json | 713aee5a13f0ba32495e146d7708d728ecb035e928a1b6b5bab9351e1e54d54a |
| build/stream-residency/clod-resident-draw-20260927-20260927-111920/result.json | 307c1b557b51059c04be69068618f21db0f166c9e9468afc08efd3f10d9ee8b6 |
| build/stream-residency/clod-resident-disabled-20260927-20260927-112035/engine.log | f0b68738e14668c7a2965d78c0bf56aa0d43e39fb11589a4f03c09d3a82f26cf |
| build/stream-residency/clod-resident-disabled-20260927-20260927-112035/harness.jsonl | 224a65b55c14210fb40c7a0873c2d6bd7ef67a815644bbf6c45c6a0a18114dd7 |
| build/stream-residency/clod-resident-disabled-20260927-20260927-112035/provenance.json | cb8ee4d6aa50892bb43f7e4ea61e7ff54863602dc03fca514654acaaeffeb55b |
| build/stream-residency/clod-resident-disabled-20260927-20260927-112035/result.json | 84f6185fda7cade6b79236b6b305ef6001b23c260aa4f8b3ab3f62506676909f |
| build/stream-residency/authored-after-clod-20260927-20260927-112142/coarse.png | cfb00a36763749ebbeff3bd7c26c51bd98b8ed40ae7314e669f6067e46657b2c |
| build/stream-residency/authored-after-clod-20260927-20260927-112142/engine.log | 40255486171f037c2c65b61525ed96fe409cee3562428e2e5d3893fab2d91ed8 |
| build/stream-residency/authored-after-clod-20260927-20260927-112142/fine.png | 805e418a10ed6af04a4419e947418eecf3b82e96528cc3fdceab4f865ed93da2 |
| build/stream-residency/authored-after-clod-20260927-20260927-112142/harness.jsonl | 7b820567b701148d3ffcfeb1d5ba8fefe0d32e3794ec94a8a827232e47215285 |
| build/stream-residency/authored-after-clod-20260927-20260927-112142/provenance.json | 141457a47b30f62a2e98159df1bd8aebe339d502982e0e636c2976cf942f37b2 |
| build/stream-residency/authored-after-clod-20260927-20260927-112142/result.json | e901667b12862a6ff5a2280a6159b9d2972c923b739fdd7cd6b478c4cef0c4ec |


## Actual authored-page worker and pass-facts checkpoint, 11:50

Source 28248174 passed the five-target build (252 scheduled steps; last printed
249), 30,683 assertions in 64 focused C++ cases without skips, and five Rust
`geometry_pass_facts` tests without ignored cases. The later mechanical Rust
formatting changes no policy. Installed stamp is clean
`28248174 2026-09-27_11:50 deploy-ps1`. Executable SHA256:
`a33ac67a98fc31c9496b205ceb1cbc965f181b45c5a884108057bc4878605c7b`;
DLL SHA256:
`e6367a5548c9b9d86ad3f95c29beb913cac4f16a5b43664d23fd5a39f9a30750`.

`authored-page-paging-20260927-20260927-115050` passed with a clean exit. One
explicit immutable RAM-backed authored fine-page job was held at epoch1 while
its independent coarse fallback stayed Present (54,088 known stored fixture
bytes). The active job retained 172,040 scheduling bytes after Abort removed the
one mesh record. Replacement epoch2 retained only its coarse record and queued
one new job alongside the cancelled active old job: two jobs, 344,080 reserved
bytes. Releasing the worker published only epoch2. The fine representation had
three Present records and 63,224 known stored fixture bytes; active/queued/ready
jobs and reservation fell to zero. Final retirement made all three records Absent
with no job debt. These values cover declared scheduling/stored contents, not
RSS, peak scratch, physical GPU freeing or generalized ownership.

The actual copied facts have valid version1/576-byte layout, required mask29 and
capabilities15. Four cascades recorded mask15 with current instance epoch and
frame. No local shadow was configured. Interior/GI cached epochs were sometimes
older, truthfully yielding pending20/28; pending16 is unsupported reflection.
The C++ observation now requires actual `wgr_render_frame` return zero, clears
facts for pending/failed frames, and calls the getter only after a successful
returned real frame. These are indirect command-recording facts: neither nonzero
survivors, fixture-specific coverage, pixels, queue completion nor all-pass
parity follows. Selected same-epoch refresh invalidation is unit-tested before
resource refusal; only actual recording restores the witness.

`clod-after-paging-20260927-20260927-115227` passed with a clean exit on the common
publication path, preserving the actual resident CLOD cuts. The disabled authored
worker control `authored-page-paging-disabled-20260927-20260927-115402` passed with
a clean exit and zero fixture payload/meshes. This is immutable RAM-page decode
and owner-queued conventional publication, not disk streaming, arbitrary retail
eligibility or automatic CLOD paging. No default worker/queue is allocated, but
24 bytes of Engine optional fields, the eight-byte Gfx optional pointer and
branches remain; default cost is not literally zero. Fine eviction/refill remains
future work, excluded from this build. The earlier ResourceGrowth failure and
narrow repeat convergence pass remain unchanged. Full ownership, physical reclaim,
all-pass, pressure, performanceAccepted and defaultReady gates remain unresolved.

Actual checkpoint SHA256 manifest:

| Artifact | SHA256 |
| --- | --- |
| build/build-authored-page-worker-20260927.log | deeba98c062fec83f3bb5a7a018967b43515955c313df51c7bcd7aea069eba6c |
| build/test-authored-page-worker-20260927.log | 6ff19c2a77478fcb73e2ee3122de248515c0fbaddb887a00ae5b95d979111090 |
| build/test-geometry-pass-facts-20260927.log | 843f6685f58eccbbcd028c7d9be124bb1604aa19296943aafce6608345947c46 |
| build/stream-residency/authored-page-paging-20260927-20260927-115050/coarse.png | 704eb21190de4e60a883646b0b0db640de2387e1a17c306b1474e247405d7443 |
| build/stream-residency/authored-page-paging-20260927-20260927-115050/engine.log | 6ef5b6ee3eb032313ead5beee421c50e4c838dfa7784906c0f120488901a0743 |
| build/stream-residency/authored-page-paging-20260927-20260927-115050/fine.png | 2fe75b77dff7f2801436f4cf0e2f66e5ec632d0746e4f1c0fcca87c5adc8d620 |
| build/stream-residency/authored-page-paging-20260927-20260927-115050/harness.jsonl | 75fa593c1feca0c7f007148167d1fc558e64aa37a98dedb6d0f8814c9efb7461 |
| build/stream-residency/authored-page-paging-20260927-20260927-115050/initial-coarse.png | a7e005813bd85d658335f26424405234f0e58c97d863877006676cf73522b928 |
| build/stream-residency/authored-page-paging-20260927-20260927-115050/provenance.json | af0ad798f021c1ad0d364463a277489b55ef38835fd248842343299a6bf754fc |
| build/stream-residency/authored-page-paging-20260927-20260927-115050/result.json | e5b751e47e2aeefd9e0a2ad4604474c829a5f6461c42d5e2eb5c4068a32b31e2 |
| build/stream-residency/clod-after-paging-20260927-20260927-115227/coarse.png | be8bcb58b105e87b288d1e4adf183a4049f87a0159f2a82c4781b0b4865df28e |
| build/stream-residency/clod-after-paging-20260927-20260927-115227/engine.log | 09ed79d871a363a2192ea6d0f2dfe74859dd9adb9f1598cd37ec2398559ea215 |
| build/stream-residency/clod-after-paging-20260927-20260927-115227/fallback.png | ab00fa66f175ec32c8a953224116190e2c71a87959268a5720160cc4cab72b24 |
| build/stream-residency/clod-after-paging-20260927-20260927-115227/fine.png | 4d78b44362396b4355dc5820a4297ab4da5ecf8b207fdc65a081ff73c36b5cab |
| build/stream-residency/clod-after-paging-20260927-20260927-115227/harness.jsonl | 9fbd4cb205afaf6e86e2fa233a08bfc7ae862c02ba1a94f3374fac36e63e2f9f |
| build/stream-residency/clod-after-paging-20260927-20260927-115227/provenance.json | c361d4a1c0577dd497339994d69b855ee0ed594c9f98658e960a8bda51faf40d |
| build/stream-residency/clod-after-paging-20260927-20260927-115227/result.json | e2ecae8e91a2388c77a1f4e900db59d027b1f51f07719f5e961db5f146ae7b44 |
| build/stream-residency/authored-page-paging-disabled-20260927-20260927-115402/engine.log | 8de26c6268c2b219fc08546c656d095d87eadd633f4eae2924eba000962013e1 |
| build/stream-residency/authored-page-paging-disabled-20260927-20260927-115402/harness.jsonl | 42b6ab2850a1cb312c081c48bb302e33e17598a3f053c03c904400e052ef96f2 |
| build/stream-residency/authored-page-paging-disabled-20260927-20260927-115402/provenance.json | 73fef6f9f20319cf1a9be254b0da901e0606560af55fa691eff35de271502456 |
| build/stream-residency/authored-page-paging-disabled-20260927-20260927-115402/result.json | be998b54c1bccf1329a1f4dca5c82c0ead873db410da533547b35e7f9a0049b4 |


## Actual one-cycle fine eviction/refill and reflection facts, 12:18

Clean installed source 3d720430 passed the five-target build (249 scheduled
steps), 16,004 assertions in 31 focused C++ cases without skips and seven Rust
pass-fact tests, including the FFI rejected-layout output canary. Installed EXE
SHA256 is `6b7aa69ae7aab27adf7bc58f27c9f01bb5a51f8d74a853d368cabc61512f7766`;
DLL SHA256 is `6f2ad6687862f7e11ba0f2a0a79fa3c0c21db367c0e2840fe4de6ce090bd504d`.

`authored-page-eviction-20260927-20260927-121822` passed with a clean exit. After
an actual successful coarse frame, generation1 fine retirement preserved the
ordered three-handle history: one Present coarse and two Absent old fine records.
A held refill was cancelled with request zero while one active job and 172,040
scheduling bytes remained. A distinct refill request queued beside it (two jobs,
344,080 reserved bytes). Generation2 published a fresh model and two new mesh
handles while preserving old identities: five historical records, three Present
and two Absent. Final Abort made all five Absent with jobs/reservation zero.
Known stored fixture contents were 64,056 bytes at the evicted/refilled/final cuts;
these are not physical GPU payload, peak scratch, RSS or reclaimed bytes. One
cycle is explicitly bounded; a second eviction returned Invalid. The independent
coarse model and fixed instance survived throughout. History is never overwritten
or shortened to conceal retired handles.

`clod-reflection-facts-20260927-20260927-121859` passed with a clean exit. Copied
facts now use version2/592 bytes and capabilities31. Its coarse observation
required29/pending12, frame34/instanceEpoch4, cascade mask15, reflectionFrame34 /
reflectionEpoch4, recordedThisFrame21. Other eviction cuts have pending0,4 or12;
older interior/GI facts remain honestly Pending. A supported reflected command
is recorded only after the actual indirect encoding helper succeeds and the pass
closes, with the same frame and retained-instance epoch. v1/576 and mixed old
layouts are refused before touching renderer or output. Recording still says
nothing about nonzero survivors, this fixture's visibility, pixels, submission,
completion or all-pass parity. A zero pending mask is limited to these recorder
contracts, not complete renderer geometry ownership or visual certification.

Disabled control `authored-eviction-disabled-20260927-20260927-122004` passed
with a clean exit and zero fixture payload/history. The same installed candidate
`default-after-eviction-reflection-20260927-disabled-20260927-122106` also passed
with a clean exit, 735 captured frames and zero dropped frames. Raw frame times
were mean48.995293877551056ms, median24.372ms, p95 166.552ms, p99 467.457ms and
max938.199ms. Tails use nearest rank ceil(n*p)-1; the median is the middle value
for this odd sample count. Returned residency was19,923, required0, geometry
730,649,324 bytes/capacity1,275,068,416, textures2,835,557,268 bytes/2,563 entries,
backend allocation5,387,195,840. Engine phases include drw:init avg17.981/p95
124.150/max664.686ms, land:gnd max569.422ms and swap max904.632ms: substantial
traversal hitches remain. This is one candidate-only Disabled/Counted/PBO-model1/
DDC0/20,000-budget diagnostic, not every normal default setting or matched-work
baseline. No performance/no-regression acceptance follows. No baseline A/B was executed: the private
raw-pair-swap helper is incompatible with the mandatory supported Deploy.ps1 path
and remains unexecuted. No performance verdict follows from these small fixtures.
Defaults remain off; RAM pages only, no disk/retail/autopaging/pressure policy.
The earlier ResourceGrowth failure and narrow repeat convergence pass remain
material limits; full ownership, physical reclaim, performanceAccepted and
defaultReady remain unresolved. Future changes are excluded from this install.

Actual checkpoint SHA256 manifest:

| Artifact | SHA256 |
| --- | --- |
| build/build-authored-eviction-reflection-20260927.log | 3ef85af47dc575530746cefdf2da720b781fa571833faec7f03d5d31ace9578a |
| build/test-authored-eviction-reflection-20260927.log | 92f842d8031df2d3888e662a6cd7a2592934b2b763fc5f01bc0f62e1a4a2f251 |
| build/test-reflection-pass-facts-20260927.log | ceaa0fc798c8ce5ac7cc5ea64fde54b84fba6b9ecb1d8a29d13c2a9e6f29e797 |
| build/stream-residency/authored-page-eviction-20260927-20260927-121822/coarse.png | d286e3abcd1ce974a13136c431ec55fb68d74a0d973c2750122327bd0227baab |
| build/stream-residency/authored-page-eviction-20260927-20260927-121822/engine.log | 506661c85fe333ac78a7a3046dbcd9ce7b15d866568e910ce5bc84a19ec92c52 |
| build/stream-residency/authored-page-eviction-20260927-20260927-121822/evicted-coarse.png | b6dc21b37e6be9310eaf7fbb098959a30aab7fd8c61914af89f6f51806fd68ee |
| build/stream-residency/authored-page-eviction-20260927-20260927-121822/fine.png | e870875b21b98ce48f24115db9a9bd749c984b8ebac697a663ad628bb8e1c510 |
| build/stream-residency/authored-page-eviction-20260927-20260927-121822/harness.jsonl | 69aeb6671e61b45c4cdaeb4bf69ed5dd854769ea9d9b9796d74745ee4f9de266 |
| build/stream-residency/authored-page-eviction-20260927-20260927-121822/initial-coarse.png | cbedeb45913f69dcaf3d974d7948241cd81dc11668eac82e7363b0bd4771e72c |
| build/stream-residency/authored-page-eviction-20260927-20260927-121822/provenance.json | 58c3a766eab088d55e70f2875a88a78f741e347f0c5e95678d88b0db02fbfdc5 |
| build/stream-residency/authored-page-eviction-20260927-20260927-121822/refilled-fine.png | 30c321aea495ad7c929f642ee5daf0e968d8644bee9a387a43e732271bc7ccaa |
| build/stream-residency/authored-page-eviction-20260927-20260927-121822/result.json | 0a651ce95f4e0b2597e1a5360b1bb290af139cb1faa6e89579217f1830864a3b |
| build/stream-residency/clod-reflection-facts-20260927-20260927-121859/coarse.png | d102a8dccbe2d9f3209a0bb65c7456e95cd405f0cd44583b4f318602a5a8906a |
| build/stream-residency/clod-reflection-facts-20260927-20260927-121859/engine.log | c1df621933dfbd7c1e29225bee1765272dd803d6ef97b5b92671e83febaa8192 |
| build/stream-residency/clod-reflection-facts-20260927-20260927-121859/fallback.png | b79db3148140d29161d5c97c80cdc78d32013936b49132c092ff51a04b2cec31 |
| build/stream-residency/clod-reflection-facts-20260927-20260927-121859/fine.png | ba6a093354b446a1283617aa6f3b88f1b9c06601fd5c11000804b80c213e0e9a |
| build/stream-residency/clod-reflection-facts-20260927-20260927-121859/harness.jsonl | 2768493f43ab654aa50ea9e1916dcb47948d7c8bc0ee8fdf8bb88e3b1a574623 |
| build/stream-residency/clod-reflection-facts-20260927-20260927-121859/provenance.json | 32aec78b0a2cfcacd7478b6be35b60bb69983432236dea509a19bec9d00f86bb |
| build/stream-residency/clod-reflection-facts-20260927-20260927-121859/result.json | c58dfe72f55a5536f3c314cf3c7ba7fc1f86436d456437dc5a4cc8678acac5be |
| build/stream-residency/authored-eviction-disabled-20260927-20260927-122004/engine.log | acc6b3debc39abaa75120e35b42d3e5af5d433780c56ad0126d7f3a828af7d99 |
| build/stream-residency/authored-eviction-disabled-20260927-20260927-122004/harness.jsonl | 8a2df6ec60c7319e80f648b28973c8ae8e632668948b6ecdffe44f8b00fa79b6 |
| build/stream-residency/authored-eviction-disabled-20260927-20260927-122004/provenance.json | bf5e9b36139e63be83933a7818ea2571dfea199a10f9c7267ae4bef59479bbf9 |
| build/stream-residency/authored-eviction-disabled-20260927-20260927-122004/result.json | 5b4550b0702980d6035b46d3a9451d7509f642709f6c8ca12eebff97ae24f40c |
| build/stream-residency/default-after-eviction-reflection-20260927-disabled-20260927-122106/archive-source-binding-proof.json | 8fe3a4332e40730fde97bc5694eb6e10c793d43d85584ed81365ab122d387677 |
| build/stream-residency/default-after-eviction-reflection-20260927-disabled-20260927-122106/engine.log | a11d31f2ff7455987afa59bed660d72df68ce2b301db68e314749caa1af70142 |
| build/stream-residency/default-after-eviction-reflection-20260927-disabled-20260927-122106/harness.jsonl | d4b9839855969e63170e184ecf928bb77290e3269f22c32f3cb8e7f9bbbcd1e1 |
| build/stream-residency/default-after-eviction-reflection-20260927-disabled-20260927-122106/paa-attempt-proof.json | 281bb4a2a21eb81d7afa18164e48ebac8bebea7f738a47450d6c743f7a5ad8bd |
| build/stream-residency/default-after-eviction-reflection-20260927-disabled-20260927-122106/paa-inflight-proof.json | 5dfc8e239c862fa3a5dd0c12aad572afa3419f36d5615da4708d5b9146b4cdad |
| build/stream-residency/default-after-eviction-reflection-20260927-disabled-20260927-122106/provenance.json | 5ba28308d7fd9e63feb6d0b25d02dcf6eed498a7752eda529df481a03b4f2941 |
| build/stream-residency/default-after-eviction-reflection-20260927-disabled-20260927-122106/registration-quota-proof.json | 20f73eb3e05f3e884a86cdc4236af31b5022d2c14137d2c906ac90552b20de16 |
| build/stream-residency/default-after-eviction-reflection-20260927-disabled-20260927-122106/result.json | 946cf16a220222776b65d739643cfb95981e32ad8c2f765a73495321a0825eda |
| build/stream-residency/default-after-eviction-reflection-20260927-disabled-20260927-122106/returned-settled.png | d4ab7614ebb98d7a5f6074364616175c6b69d89100ccbd15583f5aeb3c117e83 |
| build/stream-residency/default-after-eviction-reflection-20260927-disabled-20260927-122106/route-frame-ms.json | 1cfef78532ae7fff5178f3f5305b2f859cb056aee1cc03bffea2e7830d535bb8 |
| build/stream-residency/default-after-eviction-reflection-20260927-disabled-20260927-122106/samples.jsonl | 7b0fcc3290232e21d1724fb1cb2c5590b14169c20f6557076d38e2a274d79a57 |
| build/stream-residency/default-after-eviction-reflection-20260927-disabled-20260927-122106/warm-texture-job-proof.json | 908978856894f46faabad5ef2de34e55a089e443ff070341f8c404d27f6d0625 |

## Actual selected-CLOD RAM foundation and section-refresh checkpoint, 12:43

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

Installed pair SHA256: EXE621142A6D329861009DFD55AFF60783ACC539474FBB001EB2E5D6F0FF7D57C73,
DLL045239CF20538B91B8E939A2DA6157C4FF1FEF87A95364145CCD3A3DFABD97A4.

Actual checkpoint SHA256 manifest:

| Artifact | SHA256 |
| --- | --- |
| build/build-section-refresh-clod-ram-20260927.log | fdd475fb7d108cc3fd38c0d7fd1d38c137f25e71adf19ca770ef9e19e5f405fa |
| build/build-section-refresh-repaired-20260927.log | c3ecff4dbc5375b7903835092408625f2a5da71bbec3654acda02064842d099a |
| build/test-section-refresh-initial-compile-failure-20260927.log | e2f362d2f8c414c7150b71ca31bda608d98c195a31f1e4efada6b59ecd44631f |
| build/test-section-refresh-20260927.log | 168ec7abbc057bfeb6be71f3c16ba9ce0c0b59b36554c99d8135b9c27e941732 |
| build/test-clod-ram-worker-20260927.log | c87298bb558bb379d33e4f039cb0fcb8866f108821ac0ca19706ba5532f33915 |
| build/stream-residency/section-refresh-page-lifecycle-20260927-20260927-124430/result.json | a9e051a44ea57ff35ca7703deb146402c61d3f1e978bd0ee5d6c4d4854806eb5 |
| build/stream-residency/section-refresh-page-lifecycle-20260927-20260927-124430/provenance.json | 6454cb4fff62292d91f39da9da5553fbd481ddde35149615175f1bb0a48e7f67 |
| build/stream-residency/section-refresh-page-lifecycle-20260927-20260927-124430/engine.log | 28caa20f4e473176c395bc4fe5c45efb69b13a1fa9e882c8a803bcbe105c2819 |
| build/stream-residency/section-refresh-page-lifecycle-20260927-20260927-124430/harness.jsonl | b79b96d294ebfdc8b0b6ba1526fdc19d017d1c5797dfcb957c2af4464fa4a022 |
| build/stream-residency/section-refresh-legacy-20260927-disabled-20260927-124557/result.json | 7903379eb8352e7ed6a483fa20a6fb1d321b99f76f5f594143db81f8739ed578 |
| build/stream-residency/section-refresh-legacy-20260927-disabled-20260927-124557/provenance.json | 9b7c7db3b1de1a35f41cd14e2b030cb1bb5c864076a13d8e2aae0b9f107e41bf |
| build/stream-residency/section-refresh-legacy-20260927-disabled-20260927-124557/engine.log | 9d6422504769ebfef9613d656a8094098c6bce426eaf16d5d914f3dc85e37ace |
| build/stream-residency/section-refresh-legacy-20260927-disabled-20260927-124557/harness.jsonl | 6da2bc3ffe9e55d5f69eeafadec7e5ad263b21486fe8ba624f91d0354ce2cc18 |
| build/stream-residency/section-refresh-legacy-20260927-disabled-20260927-124557/route-frame-ms.json | b78b843224f03479eb4f869f6e253d28a0eedc6203a53dd3da817501d2b1443b |
| build/stream-residency/section-refresh-legacy-20260927-disabled-20260927-124557/returned-settled.png | 48c0a8ec1a8eafd69e55f599a8fa8c3aa82db0552614dcd882ca8653449373c7 |
| build/stream-residency/section-refresh-optimized-20260927-disabled-20260927-124850/result.json | 784ac4063e91485a5d46459cde589ea288c4dd6bcad9d034d3205713171d1d52 |
| build/stream-residency/section-refresh-optimized-20260927-disabled-20260927-124850/provenance.json | 740e9933ef2297992c0af6ce2ea3f9b8415adc2553f871b5d58ed4c7edfe86c3 |
| build/stream-residency/section-refresh-optimized-20260927-disabled-20260927-124850/engine.log | 3de62a5047ccadef68f54652bede813e94686b46a32d5050b8b7810fb05ba58e |
| build/stream-residency/section-refresh-optimized-20260927-disabled-20260927-124850/harness.jsonl | 4a8093cf5b67dfffe7af3a51c46648d71322a6402dc1fddf3ebe2c0b675acb43 |
| build/stream-residency/section-refresh-optimized-20260927-disabled-20260927-124850/route-frame-ms.json | 0fa5c396aceda91ce957147df8d06a75bbabdd34756c4bd8812c48109367ee0b |
| build/stream-residency/section-refresh-optimized-20260927-disabled-20260927-124850/returned-settled.png | 71e9340b8c67413f85ee5cbbec695ff69a9f6347ae61033b72f5e5b89ab4fe0e |

## Actual combined selected-CLOD RAM paging and table domains, 13:11

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

Installed pair SHA256: EXE0868845A5418FBD2A577D8FAB8408B797F97156CD96D7C4EBFE5BBFDC98D5931,
DLL9FF78F25D767AAF4BD29BC0DD37AE488114A7E92820C2C9D45A37FF8ECBBF3FF.

Actual checkpoint SHA256 manifest:

| Artifact | SHA256 |
| --- | --- |
| build/build-clod-paged-cull-domains-20260927.log | 98850d9f668d111e4faf998786af3d3247b5eaeed53146c6a273fdbb5e6ebf09 |
| build/build-clod-paged-cull-domains-repaired-20260927.log | 003683470c96cc13cdd928cdfd83da77bb98166ca7cdae1d315e2b626cc42aef |
| build/test-cull-table-domains-20260927.log | 6bfb3b72394d9782f4c0660491538fb0f461078fb0eb625eaca0d12a3d625a44 |
| build/test-table-upload-domains-20260927.log | 24094888fc0b496088f5d1edf324db9a1f0c312fb008da8ad8a39e55cbe58b6c |
| build/test-clod-paged-policy-20260927.log | ce06ded5bbdfef20e32c3e0c9db8f5030fc9d133d6e40925d063a527c1b24503 |
| build/stream-residency/clod-paged-eviction-cull-domains-20260927-20260927-131155/result.json | f81ada51b9191263b3651a93a4e64a839bd6850b766c50d4ca443947767a9135 |
| build/stream-residency/clod-paged-eviction-cull-domains-20260927-20260927-131155/provenance.json | bc9dc0157921c49ce714483a9f2b8bd2104549698a0efd28d183a99f324a77a9 |
| build/stream-residency/clod-paged-eviction-cull-domains-20260927-20260927-131155/engine.log | c1bcdabecda8cb1de8305c030555e89d5b3777c5d0c92ce5facacbd260745f7b |
| build/stream-residency/clod-paged-eviction-cull-domains-20260927-20260927-131155/harness.jsonl | c7d3a50a1cda4e505e6c25dd10478e9932220bdce333963f38a9a3cf21d11dae |
| build/stream-residency/authored-paged-cull-domains-20260927-20260927-131320/result.json | 0e70526c406388cc2e7e2704c4609650f5a462f9d3e3900d4b4f1bda972910db |
| build/stream-residency/authored-paged-cull-domains-20260927-20260927-131320/provenance.json | 2029083efdf1555a28f860984e195df94b97d59b789cf3302d6e3e2762a6d853 |
| build/stream-residency/authored-paged-cull-domains-20260927-20260927-131320/engine.log | a89cbda7dd20dcc2ed1cce2b7260f09b87bacb783c11a92c2b09fc0e4c15dea8 |
| build/stream-residency/authored-paged-cull-domains-20260927-20260927-131320/harness.jsonl | 3248961fd3f853154638376ec66b5a7e8a2687d59bb7811af3d5aaa59e4e18db |
| build/stream-residency/resident-clod-cull-domains-20260927-20260927-131335/result.json | e4ff0d27d2a31fd1e048c5c558db4ad3945550d49d3da58f56dba1bceb68b380 |
| build/stream-residency/resident-clod-cull-domains-20260927-20260927-131335/provenance.json | 66b5518399bea4b18ed8f4adeb427d495f35319609310b06873b98ac50bfd432 |
| build/stream-residency/resident-clod-cull-domains-20260927-20260927-131335/engine.log | 6ba2b43d2c2778c264297e8ee5da463337d818c842b41cf4e77918550372e295 |
| build/stream-residency/resident-clod-cull-domains-20260927-20260927-131335/harness.jsonl | cc6e310aa21526d5b9f1b1106209ddf5c759ea49d5ac5b0e0c2234eb5c5a4d04 |
| build/stream-residency/combined-pages-disabled-20260927-20260927-131349/result.json | f7004c917436aeef2d6b287a958206f3b0fa9892c4fdac1da18750b6e0010882 |
| build/stream-residency/combined-pages-disabled-20260927-20260927-131349/provenance.json | 3bfca9e27d8aaeaec7a9c32a05ce6cce501bb4e4ca8a3f97cb1b73a5d79ce666 |
| build/stream-residency/combined-pages-disabled-20260927-20260927-131349/engine.log | 6435f12888e2e902e174b1d12eee6b0d65fdbfe762cd0f30c7a9f0e4819bb0c5 |
| build/stream-residency/combined-pages-disabled-20260927-20260927-131349/harness.jsonl | 0bd3055df8ce15bb0b5c546408dad951d0945741adfdef238f4247398ad8dba7 |
